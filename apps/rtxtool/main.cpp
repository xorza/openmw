#include <charconv>
#include <cstdint>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <boost/program_options.hpp>

#include <components/debug/debugging.hpp>
#include <components/debug/debuglog.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/fallback/validate.hpp>
#include <components/files/configurationmanager.hpp>
#include <components/files/conversion.hpp>
#include <components/misc/constants.hpp>
#include <components/platform/platform.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/rtx/distantland.hpp>
#include <components/rtx/fogbuilder.hpp>
#include <components/rtx/lightbuilder.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>
#include <components/rtx/texturebuilder.hpp>
#include <components/rtx/upscale.hpp>

#include <components/settings/settings.hpp>
#include <components/settings/values.hpp>
#include <limits>

#include "actor.hpp"
#include "bench.hpp"
#include "benchsuite.hpp"
#include "cellchoice.hpp"
#include "cellscene.hpp"
#include "find.hpp"
#include "npc.hpp"
#include "options.hpp"
#include "picture.hpp"
#include "placement.hpp"
#include "posedactors.hpp"
#include "scene.hpp"
#include "shot.hpp"
#include "stagedworld.hpp"
#include "textures.hpp"
#include "validationchoice.hpp"
#include "verify.hpp"
#include "view.hpp"
#include "views.hpp"
#include "world.hpp"

namespace RtxTool
{
    namespace
    {
        namespace bpo = boost::program_options;

        using StringsVector = std::vector<std::string>;

        constexpr std::string_view applicationName = "RtxTool";

        /// Whether the validation layers load without anyone asking.
        ///
        /// **On outside a Release build**, because the alternative is what this fork just spent an
        /// afternoon on: a window that stuttered and froze with nothing in the log, whose cause was
        /// a ray query built with its end behind its start — undefined, silent, and named outright
        /// by GPU-assisted validation the first time it was switched on. A rule the layers can check
        /// is one nobody should have to think to check for.
        ///
        /// They are not free. GPU-assisted validation instruments every shader and costs roughly
        /// half the frame rate, and the layers themselves allocate on the frame path, which is why
        /// the test that counts allocations builds its own device without them. `--validation=false`
        /// turns them off for a measurement, and a Release build never had them.
#ifdef OPENMW_RTX_VALIDATION_BY_DEFAULT
        constexpr bool sValidationByDefault = true;
#else
        constexpr bool sValidationByDefault = false;
#endif

        /// Which layers a run wants, from what the command line asked for.
        ///
        /// Shared because `info` and every other command have to agree: a device that reported its
        /// limits under one set of layers and traced under another would be describing something
        /// nobody ran.
        ///
        /// @param windowed whether this run opens a window, which is the one place GPU-assisted
        ///        validation cannot be left on.
        Rtx::ValidationOptions validationFrom(const bpo::variables_map& variables, bool windowed)
        {
            // Whether a switch was set matters as much as what it says, so both come across.
            const auto asSwitch = [&](const char* name) {
                return CommandSwitch{ variables[name].as<bool>(), !variables[name].defaulted() };
            };

            return RtxTool::chooseValidation(
                asSwitch("validation"), asSwitch("sync-validation"), asSwitch("gpu-validation"), windowed);
        }

        /// Reports go to the unprefixed stream.
        ///
        /// `Debug::wrapApplication` routes `std::cout` through the log formatter, which stamps every
        /// line with a time and a level. That is right for a game and wrong for a tool whose output
        /// is meant to be read, diffed, or piped into something that parses it.
        std::ostream& out()
        {
            return Debug::getRawStdout();
        }

        /// Parses `WIDTHxHEIGHT`.
        std::pair<std::uint32_t, std::uint32_t> parseSize(std::string_view text)
        {
            const std::size_t cross = text.find('x');
            std::uint32_t width = 0;
            std::uint32_t height = 0;

            const bool ok = cross != std::string_view::npos
                && std::from_chars(text.data(), text.data() + cross, width).ec == std::errc()
                && std::from_chars(text.data() + cross + 1, text.data() + text.size(), height).ec == std::errc();

            if (!ok || width == 0 || height == 0)
                throw std::runtime_error("not a size: " + std::string(text));

            return { width, height };
        }

        Rtx::Upscale parseUpscale(std::string_view text)
        {
            const std::optional<Rtx::Upscale> named = Rtx::upscaleNamed(text);
            if (!named.has_value())
                throw std::runtime_error("not an upscale mode: " + std::string(text));

            return *named;
        }

        Rtx::Preset parsePreset(std::string_view text)
        {
            const std::optional<Rtx::Preset> named = Rtx::presetNamed(text);
            if (!named.has_value())
                throw std::runtime_error("not a Ray Reconstruction preset: " + std::string(text));

            return *named;
        }

        /// What `--exposure` asked for: a number to hold it at, or nothing to measure it.
        std::optional<float> parseExposure(std::string_view text)
        {
            if (text == "auto")
                return std::nullopt;

            float value = 0.0f;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc() || end != text.data() + text.size() || !(value > 0.0f))
                throw std::runtime_error("not an exposure: " + std::string(text));

            return value;
        }

        void printUsage(const bpo::options_description& options)
        {
            out() << "Drives the experimental ray tracing renderer without the game window.\n\n"
                     "Usage: openmw-rtxtool <command> [options]\n\n"
                     "Commands:\n"
                     "  info     report the device this renderer would run on\n"
                     "  scene    read a cell and report what the renderer would be handed\n"
                     "  shot     render a cell and write a PNG, with no window\n"
                     "  view     open a window on a cell and fly around it\n"
                     "  bench    time a run of frames at each of a list of places\n"
                     "  textures every texture a cell uses, vanilla beside de-lit, as one sheet\n"
                     "  doll     the inventory doll of one person, traced against a scene of its own\n"
                     "  map      one local-map tile of a cell, traced straight down\n"
                     "  verify   render every view and say what moved since the last run\n\n"
                     "With no arguments at all: a window on the ship at Seyda Neen, where the game starts.\n\n"
                  << options;
        }

        /// Applies every terrain option at once: whether the world pages, how far, and whether
        /// the distant ground carries anything.
        ///
        /// **Together, because they are read together everywhere.** Paging without distance is a
        /// quad tree that barely leaves the active grid, distance without paging is a radius
        /// `TerrainGrid` ignores, and statics without either are statics on ground that does not
        /// exist — so a command that set one and forgot the rest would be rendering a different
        /// world from the one its line asked for.
        void pageTerrainFrom(World& world, const bpo::variables_map& variables)
        {
            // **Written to the setting and not only handed to the terrain**, because the air reads
            // the same number. Fog is a half-life measured across some distance; tuned to seven
            // thousand units it swallows a world built to thirty, and a ring of ground four cells
            // out then renders identically to one built none — which is how this came to be one
            // number rather than two.
            const float cells = variables["distant-cells"].as<float>();

            // **On by default, and still nameable.** A radius means nothing without the paged
            // world — `TerrainGrid` builds the staged cells and nothing else, which is the
            // three-cell square of ground with sea to the horizon `--distant-terrain=false` puts
            // back. That is what the flag is for now: an A/B against the world the game builds,
            // rather than a way to reach ground that only the grid could make.
            const bool paged = variables["distant-terrain"].as<bool>();
            world.pageTerrain(paged);
            world.pageStatics(variables["distant-statics"].as<bool>());

            // **The air follows the world that is actually built, and the grid does not honour a
            // radius.** `TerrainGrid` makes the staged cells and nothing else, so a run without the
            // paged world left at four cells of reach would see clearly to thirty thousand units and
            // find the edge of a three-cell square there. Zero hands the air back to `viewing
            // distance`, which is what that square was always lit for.
            Settings::rtx().mDistantLandCells.set(paged ? cells : 0.0f);
            world.setTerrainViewDistance(Rtx::distantLandReach());
        }

        /// Who stands in the region, from the command line. **One reading of it**, because a
        /// report that described a differently populated cell than the one `shot` renders is the
        /// drift this tool exists to catch.
        ActorRequest actorsFrom(const bpo::variables_map& variables)
        {
            return ActorRequest{
                .mCreatures = variables["actor"].as<StringsVector>(),
                .mPeople = variables["npc"].as<StringsVector>(),
                .mSeconds = variables["actor-time"].as<float>(),
                .mResidents = variables["people"].as<bool>(),
                .mProps = variables["props"].as<bool>(),
                .mClothes = variables["clothes"].as<bool>(),
            };
        }

        /// When and in what weather the region stands, likewise.
        StagingRequest stagingFrom(const bpo::variables_map& variables)
        {
            StagingRequest request;
            request.mWeather = variables["weather"].as<std::string>();
            request.mHour = variables["hour"].as<float>();
            request.mDay = variables["day"].as<int>();
            request.mFieldOfView = variables["fov"].as<float>();

            // Where to stand is left out: a report is not taken from anywhere, and the two commands
            // that read one derive the camera from the region's own bounds.
            return request;
        }

        /// What a picture inside the interface is asked for, from the command line.
        ///
        /// @param width the size to write at where `--size` said nothing. **Its own default and not
        ///        the shot's**, because a doll and a map tile are pictures rather than frames: the
        ///        game draws one at 512 by 1024 and the other square.
        PictureRequest pictureFrom(const bpo::variables_map& variables, const std::filesystem::path& resources,
            std::uint32_t width, std::uint32_t height)
        {
            const auto [asked, high] = parseSize(variables["size"].as<std::string>());

            PictureRequest request;
            request.mOutput = variables["out"].as<std::string>();
            request.mShaderDirectory = resources / "rtx" / "shaders";
            request.mWidth = variables["size"].defaulted() ? width : asked;
            request.mHeight = variables["size"].defaulted() ? height : high;
            request.mSeconds = variables["actor-time"].as<float>();
            request.mDressed = variables["clothes"].as<bool>();
            request.mOrigin = parseVec3(variables["pos"].as<std::string>(), "--pos");
            request.mTarget = parseVec3(variables["look"].as<std::string>(), "--look");
            return request;
        }

        int runInfo(const std::filesystem::path& shaderDirectory, const Rtx::ValidationOptions& validation)
        {
            // A one-pixel target: this reports on a device rather than drawing with it, and the
            // default would spend fifty megabytes of images to print a page of text.
            //
            // **The shaders are still named, because standing a renderer up compiles one.**
            // Reporting on a device is not a reason to build half a renderer, and a build whose
            // shaders are missing should say so here rather than at the first frame asked for.
            std::string reason;
            const std::unique_ptr<Rtx::Renderer> renderer = Rtx::createRenderer(
                Rtx::RendererOptions{
                    .mShaderDirectory = shaderDirectory, .mWidth = 1, .mHeight = 1, .mValidation = validation },
                reason);
            if (renderer == nullptr)
            {
                out() << reason << '\n';
                return 1;
            }

            out() << renderer->describeDevice();
            return 0;
        }

        /// Reads a cell and places all of it, lights included.
        ///
        /// An interior's illumination is its own lamps over its own `AMBI`; an exterior's is the sky
        /// and the weather, which the cell says nothing about and the clock decides.
        int runShot(World& world, const std::string& cellSpec, const Rtx::ValidationOptions& validation,
            ShotRequest request, const ActorRequest& actors)
        {
            const ESM::Cell* cell = findCellOrComplain(world, cellSpec);
            if (cell == nullptr)
                return 1;

            // Held for the whole render: the extractor keys its meshes on node pointers, and actors
            // freed while the scene still names them is a dangling identity.
            StagedWorld staged(world, *cell,
                StagingRequest{
                    .mWeather = request.mWeather,
                    .mHour = request.mHour,
                    .mDay = request.mDay,
                    .mFieldOfView = request.mFieldOfView,
                    .mOrigin = request.mOrigin,
                    .mTarget = request.mTarget,
                },
                actors);

            request.mLighting = staged.getLighting();
            request.mOrigin = staged.getPlacement().mOrigin;
            request.mTarget = staged.getPlacement().mTarget;
            request.mMotion = staged.getMotion();

            printCellHeading(*cell);

            if (staged.getActorCount() > 0 || staged.getPropCount() > 0)
            {
                const Rtx::ExtractionStats& settled = staged.getSettled();
                out() << "actors:     " << staged.getActorCount() << " placed, " << settled.mDeformed
                      << " deforming drawables\n"
                      << "props:      " << staged.getPropCount() << " live, " << settled.mEmitters
                      << " emitters holding " << settled.mSprites << " particles\n";
            }

            out() << '\n';

            return renderShot(staged.getScene(), world.getImageManager(), validation, request);
        }

        int runView(World& world, const std::string& cellSpec, const Rtx::ValidationOptions& validation,
            ViewRequest request, const ActorRequest& actors)
        {
            const ESM::Cell* cell = findCellOrComplain(world, cellSpec);
            if (cell == nullptr)
                return 1;

            printCellHeading(*cell);

            return runWindow(world, *cell, validation, std::move(request), actors);
        }

        /// Where the command line and the view file meet.
        ///
        /// A named view supplies the cell and usually the camera; anything given explicitly on the
        /// command line wins over it, so a view is a starting point rather than a straitjacket.
        struct Chosen
        {
            std::string mCell;
            std::string mTitle;

            /// The view file's id and note, empty where nothing named one.
            std::string mView;
            std::string mNote;

            std::optional<osg::Vec3f> mOrigin;
            std::optional<osg::Vec3f> mTarget;
        };

        /// Where someone starts when they have said nothing about where: the ship at Seyda Neen,
        /// where the game starts and the one place every player of it has stood.
        constexpr std::string_view sDefaultView = "seyda-neen-ship";

        /// The view a run names, or null where it named none and gave a cell instead.
        ///
        /// Separated out because `Chosen` is built from it in one go below: an aggregate assembled
        /// in two stages cannot name every field in its initialiser, and the compiler is right to
        /// say so.
        const View* findChosenView(
            const bpo::variables_map& variables, const std::filesystem::path& resources, std::vector<View>& views)
        {
            std::string name = variables["view"].as<std::string>();
            if (name.empty())
            {
                if (!variables["cell"].as<std::string>().empty())
                    return nullptr;

                name = sDefaultView;
            }

            views = loadViews(resources / "rtx" / "views.cfg");
            const View* view = findView(views, name);
            if (view != nullptr)
                return view;

            std::string known;
            for (const View& candidate : views)
                known += "\n  " + candidate.mName + "   " + candidate.mNote;

            throw std::runtime_error("no view is called \"" + name + "\". These are:" + known);
        }

        Chosen chooseView(const bpo::variables_map& variables, const std::filesystem::path& resources)
        {
            // Holds what the view below points into, for as long as this function needs it.
            std::vector<View> views;
            const View* view = findChosenView(variables, resources, views);

            // Anything given on the command line wins over the view, which is why the two optionals
            // are read first and the view is only consulted where they came back empty.
            const std::optional<osg::Vec3f> origin = parseVec3(variables["pos"].as<std::string>(), "--pos");
            const std::optional<osg::Vec3f> target = parseVec3(variables["look"].as<std::string>(), "--look");

            if (view == nullptr)
                return Chosen{
                    .mCell = variables["cell"].as<std::string>(),
                    .mTitle = "OpenMW RTX",
                    .mView = {},
                    .mNote = {},
                    .mOrigin = origin,
                    .mTarget = target,
                };

            return Chosen{
                .mCell = view->mCell,
                .mTitle = "OpenMW RTX - " + view->mName,
                .mView = view->mName,
                .mNote = view->mNote,
                .mOrigin = origin.has_value() ? origin : view->mOrigin,
                .mTarget = target.has_value() ? target : view->mTarget,
            };
        }

        /// The places a profiling run visits, in the order it visits them.
        ///
        /// **`--views` beats `--suite`, and both name entries in `views.cfg`.** A suite is a list
        /// written down so a run can be repeated without remembering it; a list on the command line
        /// is the same thing for one run. Neither carries coordinates: those live with the view, so
        /// the frame a picture is taken of and the frame a number is measured on stay one frame.
        std::vector<View> chooseBenchViews(
            const bpo::variables_map& variables, const std::filesystem::path& resources, std::string& suiteName)
        {
            const std::vector<View> views = loadViews(resources / "rtx" / "views.cfg");
            const std::string named = variables["views"].as<std::string>();

            if (named == "all")
                return views;

            std::vector<std::string> wanted;
            if (named.empty())
            {
                suiteName = variables["suite"].as<std::string>();

                const std::vector<BenchSuite> suites = loadSuites(resources / "rtx" / "benches.cfg");
                const BenchSuite* suite = findSuite(suites, suiteName);
                if (suite == nullptr)
                {
                    std::string known;
                    for (const BenchSuite& candidate : suites)
                        known += "\n  " + candidate.mName + "   " + candidate.mNote;

                    throw std::runtime_error("no suite is called \"" + suiteName + "\". These are:" + known);
                }

                wanted = suite->mViews;
            }
            else
                wanted = splitNames(named);

            std::vector<View> chosen;
            chosen.reserve(wanted.size());
            for (const std::string& name : wanted)
            {
                const View* view = findView(views, name);
                if (view == nullptr)
                    throw std::runtime_error("no view is called \"" + name + "\"; --list-views prints them");

                chosen.push_back(*view);
            }

            if (chosen.empty())
                throw std::runtime_error("nothing to profile: no view was named");

            return chosen;
        }

        int runListViews(const std::filesystem::path& resources)
        {
            for (const View& view : loadViews(resources / "rtx" / "views.cfg"))
                out() << "  " << view.mName << "\n      " << view.mCell << "\n      " << view.mNote << '\n';

            return 0;
        }

        int dispatch(int argc, char* argv[])
        {
            Platform::init();

            // The verb is taken straight off the command line rather than declared as a positional.
            // `ConfigurationManager::readConfiguration` walks the variables map and looks every key
            // up in the options description it was handed, so a key that is deliberately not in that
            // description — which is what a hidden positional is — makes it throw.
            // A window is what this is for, so that is what it does when nobody says otherwise —
            // with no arguments at all, or with only options and no verb.
            const bool hasVerb = argc >= 2 && argv[1][0] != '-';
            const std::string command = hasVerb ? argv[1] : "view";

            bpo::options_description options = makeOptionsDescription(sValidationByDefault);

            // Boost skips the first token as the program name; here that token is the verb.
            // Boost skips the first token as the program name; when there is a verb, that token is
            // the verb.
            bpo::variables_map variables;
            bpo::store(hasVerb ? bpo::command_line_parser(argc - 1, argv + 1).options(options).run()
                               : bpo::command_line_parser(argc, argv).options(options).run(),
                variables);
            bpo::notify(variables);

            if (variables.count("help") > 0)
            {
                printUsage(options);
                return 0;
            }

            Files::ConfigurationManager config;
            config.processPaths(variables, std::filesystem::current_path());
            config.readConfiguration(variables, options);
            Debug::setupLogging(config.getLogPath(), applicationName);
            Settings::Manager::load(config);

            const std::filesystem::path resources = variables["resources"].as<Files::MaybeQuotedPath>();

            if (command == "info")
            {
                const Rtx::ValidationOptions validation = validationFrom(variables, false);

                return runInfo(resources / "rtx" / "shaders", validation);
            }

            if (variables["list-views"].as<bool>())
                return runListViews(resources);

            if (command == "textures")
            {
                const Chosen chosen = chooseView(variables, resources);
                World world(config, variables, resources);
                pageTerrainFrom(world, variables);

                const ESM::Cell* cell = findCellOrComplain(world, chosen.mCell);
                if (cell == nullptr)
                    return 1;

                return runTextures(world, *cell, stagingFrom(variables), actorsFrom(variables),
                    variables["out"].as<std::string>(), variables["delight"].as<float>());
            }

            if (command == "doll")
            {
                // **The first of them, because `--npc` is the row `shot` stands up.** One picture is
                // of one person, and the option is shared rather than duplicated so that the same
                // name reaches the same record either way.
                //
                // Read before the content files are, so a run that named nobody says so in the time
                // it takes to print a line.
                const StringsVector people = variables["npc"].as<StringsVector>();
                if (people.empty())
                {
                    out() << "doll needs somebody: --npc=<id>. `openmw-rtxtool scene --find=<text>` finds one.\n";
                    return 1;
                }

                const PictureRequest request = pictureFrom(variables, resources, 512, 1024);

                World world(config, variables, resources);

                const ESM::NPC* npc = findNpc(world, people.front());
                if (npc == nullptr)
                {
                    out() << "no NPC record is called \"" << people.front() << "\".\n";
                    return 1;
                }

                return runDoll(world, *npc, request);
            }

            if (command == "map")
            {
                const Chosen chosen = chooseView(variables, resources);
                const PictureRequest request = pictureFrom(variables, resources, 1024, 1024);

                World world(config, variables, resources);
                pageTerrainFrom(world, variables);

                const ESM::Cell* cell = findCellOrComplain(world, chosen.mCell);
                if (cell == nullptr)
                    return 1;

                return runMap(world, *cell, stagingFrom(variables), actorsFrom(variables), request);
            }

            if (command == "scene")
            {
                const Chosen chosen = chooseView(variables, resources);
                World world(config, variables, resources);
                pageTerrainFrom(world, variables);

                const ESM::Cell* cell = findCellOrComplain(world, chosen.mCell);
                if (cell == nullptr)
                    return 1;

                const std::string needle = variables["find"].as<std::string>();
                if (!needle.empty())
                    return runFind(world, *cell, needle);

                return runScene(
                    world, *cell, stagingFrom(variables), actorsFrom(variables), variables["twice"].as<bool>());
            }

            if (command == "verify")
            {
                const auto [width, height] = parseSize(variables["size"].as<std::string>());

                VerifyRequest request;
                request.mViews = loadViews(resources / "rtx" / "views.cfg");
                request.mShaderDirectory = resources / "rtx" / "shaders";
                request.mOut = variables["out"].defaulted() ? "verify" : variables["out"].as<std::string>();
                request.mAgainst = variables["against"].as<std::string>();
                request.mWidth = width;
                request.mHeight = height;
                request.mFieldOfView = variables["fov"].as<float>();
                request.mDelight = variables["delight"].as<float>();
                request.mFilter = variables["filter"].as<bool>();
                request.mExposure = parseExposure(variables["exposure"].as<std::string>());
                request.mWeather = variables["weather"].as<std::string>();
                request.mHour = variables["hour"].as<float>();
                request.mDay = variables["day"].as<int>();
                request.mActors = actorsFrom(variables);

                // **Off unless somebody asked**, for the reason `bench` gives it: GPU-assisted
                // validation instruments every shader, and a picture drawn under an instrumented
                // shader is not the picture the next run will be compared against.
                const bool asked = !variables["validation"].defaulted() || !variables["sync-validation"].defaulted()
                    || !variables["gpu-validation"].defaulted();

                const Rtx::ValidationOptions validation
                    = asked ? validationFrom(variables, false) : Rtx::ValidationOptions{};

                World world(config, variables, resources);
                pageTerrainFrom(world, variables);

                return runVerify(world, validation, request);
            }

            if (command == "bench")
            {
                const auto [width, height] = parseSize(variables["size"].as<std::string>());

                std::string suite;
                BenchRequest request;
                request.mViews = chooseBenchViews(variables, resources, suite);
                request.mSuite = suite;
                request.mShaderDirectory = resources / "rtx" / "shaders";
                request.mJson = variables["json"].as<std::string>();
                request.mPerfControl = variables["perf-control"].as<std::string>();
                request.mWidth = width;
                request.mHeight = height;
                request.mFieldOfView = variables["fov"].as<float>();
                request.mSeconds = variables["seconds"].as<float>();
                request.mWarmup = variables["warmup"].as<float>();
                request.mFrames = variables["frames"].as<std::uint32_t>();
                request.mWindow = variables["window"].as<bool>();
                request.mUpscale = parseUpscale(variables["upscale"].as<std::string>());
                request.mPreset = parsePreset(variables["preset"].as<std::string>());
                request.mDelight = variables["delight"].as<float>();
                request.mFilter = variables["filter"].as<bool>();
                request.mExposure = parseExposure(variables["exposure"].as<std::string>());
                request.mWeather = variables["weather"].as<std::string>();
                request.mHour = variables["hour"].as<float>();
                request.mDay = variables["day"].as<int>();
                request.mActors = ActorRequest{
                    .mCreatures = variables["actor"].as<std::vector<std::string>>(),
                    .mPeople = variables["npc"].as<std::vector<std::string>>(),
                    .mSeconds = variables["actor-time"].as<float>(),
                    .mResidents = variables["people"].as<bool>(),
                    .mProps = variables["props"].as<bool>(),
                    .mClothes = variables["clothes"].as<bool>(),
                };

                // **Off unless somebody asked, whatever the build default is.** The layers cost
                // between a tenth and half the frame rate, and a profiling run that quietly
                // measured one under instrumentation is worse than no run at all: it produces a
                // number, and the number is wrong.
                const bool asked = !variables["validation"].defaulted() || !variables["sync-validation"].defaulted()
                    || !variables["gpu-validation"].defaulted();

                const Rtx::ValidationOptions validation
                    = asked ? validationFrom(variables, request.mWindow) : Rtx::ValidationOptions{};

                World world(config, variables, resources);
                pageTerrainFrom(world, variables);

                return runBench(world, validation, request);
            }

            if (command == "shot" || command == "view")
            {
                const auto [width, height] = parseSize(variables["size"].as<std::string>());

                // With nothing on the command line, the ship at Seyda Neen: where the game starts,
                // and the one place every player of this game has stood.
                const Chosen chosen = chooseView(variables, resources);

                const Rtx::ValidationOptions validation = validationFrom(variables, command == "view");

                const ActorRequest actors = actorsFrom(variables);

                World world(config, variables, resources);
                pageTerrainFrom(world, variables);

                if (command == "view")
                {
                    ViewRequest request;
                    request.mTitle = chosen.mTitle;
                    request.mView = chosen.mView;
                    request.mNote = chosen.mNote;
                    request.mCell = chosen.mCell;
                    request.mShaderDirectory = resources / "rtx" / "shaders";
                    request.mScreenshotDirectory = config.getScreenshotPath();
                    request.mWidth = width;
                    request.mHeight = height;
                    request.mFieldOfView = variables["fov"].as<float>();
                    request.mOrigin = chosen.mOrigin;
                    request.mTarget = chosen.mTarget;
                    request.mFrames = variables["frames"].as<std::uint32_t>();
                    request.mShowAlbedo = variables["albedo"].as<bool>();
                    request.mFilter = variables["filter"].as<bool>();
                    request.mExposure = parseExposure(variables["exposure"].as<std::string>());
                    request.mUpscale = parseUpscale(variables["upscale"].as<std::string>());
                    request.mPreset = parsePreset(variables["preset"].as<std::string>());
                    request.mDelight = variables["delight"].as<float>();
                    request.mWeather = variables["weather"].as<std::string>();
                    request.mHour = variables["hour"].as<float>();
                    request.mDay = variables["day"].as<int>();

                    return runView(world, chosen.mCell, validation, request, actors);
                }

                ShotRequest request;
                request.mOutput = variables["out"].as<std::string>();
                request.mShaderDirectory = resources / "rtx" / "shaders";
                request.mWidth = width;
                request.mHeight = height;
                request.mFieldOfView = variables["fov"].as<float>();
                request.mOrigin = chosen.mOrigin;
                request.mTarget = chosen.mTarget;
                request.mShowAlbedo = variables["albedo"].as<bool>();
                request.mTail = variables["tail"].as<bool>();
                request.mDump = variables["dump"].as<std::string>();
                request.mJitter = variables["jitter"].as<bool>();
                request.mFilter = variables["filter"].as<bool>();
                request.mExposure = parseExposure(variables["exposure"].as<std::string>());
                request.mDelight = variables["delight"].as<float>();
                request.mWeather = variables["weather"].as<std::string>();
                request.mHour = variables["hour"].as<float>();
                request.mDay = variables["day"].as<int>();
                request.mUpscale = parseUpscale(variables["upscale"].as<std::string>());
                request.mPreset = parsePreset(variables["preset"].as<std::string>());

                // **A reference cannot be built through a denoiser.** `--accumulate` averages frames
                // towards the truth and Ray Reconstruction resolves each of them towards its own
                // opinion, so a thousand of those converge on the network rather than on the
                // integral — the same argument `mFilter` carries, one denoiser along. Turned off
                // rather than refused, because the default is on and nobody asking for a reference
                // is asking for this; someone who names `--upscale` too gets what they named.
                if (variables["accumulate"].as<std::uint32_t>() > 0 && variables["upscale"].defaulted())
                    request.mUpscale = Rtx::Upscale::Off;
                request.mRepeat = variables["repeat"].as<std::uint32_t>();
                request.mAccumulate = variables["accumulate"].as<std::uint32_t>();

                return runShot(world, chosen.mCell, validation, request, actors);
            }

            out() << "Unknown command: " << command << "\n\n";
            printUsage(options);
            return 1;
        }

        int run(int argc, char* argv[])
        {
            // Failures are reported here rather than left to `Debug::wrapApplication`, which puts up
            // an SDL message box when stdin is not a terminal. This tool is meant to be usable over
            // ssh and from a script, where a dialog nobody can see is a hang.
            try
            {
                return dispatch(argc, argv);
            }
            catch (const std::exception& e)
            {
                Debug::getRawStderr() << "openmw-rtxtool: " << e.what() << '\n';
                return 1;
            }
        }
    }
}

int main(int argc, char* argv[])
{
    // **Never a box.** This is a developer harness: it is run from a shell or a task runner, its
    // output is read, and a dialog waiting for a click is a run that never finishes — which for
    // something whose whole point is to be run in a loop is the tool not working.
    Debug::setFatalDialogs(false);

    return Debug::wrapApplication(RtxTool::run, argc, argv, RtxTool::applicationName);
}
