#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/program_options.hpp>

#include <components/debug/debugging.hpp>
#include <components/debug/debuglog.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/fallback/validate.hpp>
#include <components/files/configurationmanager.hpp>
#include <components/files/conversion.hpp>
#include <components/misc/constants.hpp>
#include <components/platform/platform.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/rtx/fogbuilder.hpp>
#include <components/rtx/lightbuilder.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/reorder.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>
#include <components/rtx/texturebuilder.hpp>
#include <components/rtx/upscale.hpp>
#include <components/rtxbench/benchrecord.hpp>
#include <components/rtxbench/benchspec.hpp>
#include <components/sceneutil/offscreenframing.hpp>
#include <components/settings/settings.hpp>
#include <components/settings/values.hpp>

#include "actor.hpp"
#include "benchsuite.hpp"
#include "cellchoice.hpp"
#include "cellscene.hpp"
#include "find.hpp"
#include "framerequest.hpp"
#include "hosted.hpp"
#include "npc.hpp"
#include "options.hpp"
#include "picture.hpp"
#include "placement.hpp"
#include "posedactors.hpp"
#include "scene.hpp"
#include "stagedworld.hpp"
#include "textures.hpp"
#include "validationchoice.hpp"
#include "verbs.hpp"
#include "verify.hpp"
#include "view.hpp"
#include "viewpoint.hpp"
#include "views.hpp"
#include "world.hpp"

namespace RtxTool
{
    namespace
    {
        namespace bpo = boost::program_options;

        using StringsVector = std::vector<std::string>;

        constexpr std::string_view applicationName = "RtxTool";

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

        Rtx::Reorder parseReorder(std::string_view text)
        {
            const std::optional<Rtx::Reorder> named = Rtx::reorderNamed(text);
            if (!named.has_value())
                throw std::runtime_error("not a reorder mode: " + std::string(text));

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
            world.setTerrainViewDistance(landReach());
        }

        /// Who stands in the region, from the command line. **One reading of it**, because a
        /// report that described a differently populated cell than the one `shot` renders is the
        /// drift this tool exists to catch.
        /// The layers a run that will be measured or compared gets, which is none unless it asked.
        ///
        /// **Off unless somebody asked, whatever the build default is.** The layers cost between a
        /// tenth and half the frame rate, and a profiling run that quietly measured one under
        /// instrumentation is worse than no run at all: it produces a number, and the number is
        /// wrong. GPU-assisted validation instruments every shader besides, so a picture drawn under
        /// one is not the picture the next run will be compared against.
        Rtx::ValidationOptions validationForMeasuring(const bpo::variables_map& variables, bool windowed)
        {
            const bool asked = !variables["validation"].defaulted() || !variables["sync-validation"].defaulted()
                || !variables["gpu-validation"].defaulted();

            return asked ? validationFrom(variables, windowed) : Rtx::ValidationOptions{};
        }

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

        /// What `--hour` named, or nothing where it was left at its default. `hourFor` is the rule
        /// this feeds.
        std::optional<float> hourGiven(const bpo::variables_map& variables)
        {
            if (variables["hour"].defaulted())
                return std::nullopt;

            return variables["hour"].as<float>();
        }

        /// What `--weather` named, or nothing where it was left at its default.
        std::optional<std::string> weatherGiven(const bpo::variables_map& variables)
        {
            if (variables["weather"].defaulted())
                return std::nullopt;

            return variables["weather"].as<std::string>();
        }

        /// `hourFor` and `weatherFor` over a run's whole list of places: a condition named on the
        /// command line is every place's, and none of them keeps its own.
        void applyConditions(const bpo::variables_map& variables, std::vector<View>& views)
        {
            const std::optional<float> hour = hourGiven(variables);
            const std::optional<std::string> weather = weatherGiven(variables);

            for (View& view : views)
            {
                if (hour.has_value())
                    view.mHour = hour;

                if (weather.has_value())
                    view.mWeather = weather;
            }
        }

        /// What every command is handed: the line it was given, the configuration that line was
        /// read against, and where the resources are.
        struct Command
        {
            const bpo::variables_map& mVariables;
            Files::ConfigurationManager& mConfig;
            const std::filesystem::path& mResources;
        };

        /// The whole of a `FrameRequest`, from the command line.
        ///
        /// @param hour,weather what the world stands under. `chooseView` decides them for a single
        ///        place, and `applyConditions` for a run of them — so they are passed rather than
        ///        read here.
        FrameRequest frameFrom(const Command& command, float hour, const std::string& weather)
        {
            const bpo::variables_map& variables = command.mVariables;
            const auto [width, height] = parseSize(variables["size"].as<std::string>());

            FrameRequest request;
            request.mShaderDirectory = command.mResources / "rtx" / "shaders";
            request.mCacheDirectory = command.mConfig.getCachePath();
            request.mWidth = width;
            request.mHeight = height;
            request.mFieldOfView = variables["fov"].as<float>();
            request.mUpscale = parseUpscale(variables["upscale"].as<std::string>());
            request.mPreset = parsePreset(variables["preset"].as<std::string>());
            request.mReorder = parseReorder(variables["reorder"].as<std::string>());
            request.mDelight = variables["delight"].as<float>();
            request.mFilter = variables["filter"].as<bool>();
            request.mShowAlbedo = variables["albedo"].as<bool>();
            request.mJitter = variables["jitter"].as<bool>();
            request.mCountCrossings = variables["crossings"].as<bool>();
            request.mExposure = parseExposure(variables["exposure"].as<std::string>());
            request.mWeather = weather;
            request.mHour = hour;
            request.mDay = variables["day"].as<int>();
            request.mActors = actorsFrom(variables);

            return request;
        }

        /// What a picture inside the interface is asked for, from the command line.
        ///
        /// @param width the size to write at where `--size` said nothing. **Its own default and not
        ///        the shot's**, because a doll and a map tile are pictures rather than frames: the
        ///        game draws one at 512 by 1024 and the other square.
        PictureRequest pictureFrom(const Command& command, std::uint32_t width, std::uint32_t height)
        {
            const bpo::variables_map& variables = command.mVariables;
            const auto [asked, high] = parseSize(variables["size"].as<std::string>());

            PictureRequest request;
            request.mOutput = variables["out"].as<std::string>();
            request.mShaderDirectory = command.mResources / "rtx" / "shaders";
            request.mCacheDirectory = command.mConfig.getCachePath();
            request.mWidth = variables["size"].defaulted() ? width : asked;
            request.mHeight = variables["size"].defaulted() ? height : high;
            request.mSeconds = variables["actor-time"].as<float>();
            request.mDressed = variables["clothes"].as<bool>();
            request.mOrigin = parseVec3(variables["pos"].as<std::string>(), "--pos");
            request.mTarget = parseVec3(variables["look"].as<std::string>(), "--look");
            return request;
        }

        int runInfo(const Command& command, const Rtx::ValidationOptions& validation)
        {
            // A one-pixel target: this reports on a device rather than drawing with it, and the
            // default would spend fifty megabytes of images to print a page of text.
            //
            // **The shaders are still named, because standing a renderer up compiles one.**
            // Reporting on a device is not a reason to build half a renderer, and a build whose
            // shaders are missing should say so here rather than at the first frame asked for.
            //
            // **And the cache is the one every other command fills**, since compiling those
            // pipelines is most of what this verb waits for. A run that named no cache compiled
            // them from source, kept nothing, and left the next `shot` to compile them again.
            std::string reason;
            const std::unique_ptr<Rtx::Renderer> renderer = Rtx::createRenderer(
                Rtx::RendererOptions{
                    .mShaderDirectory = command.mResources / "rtx" / "shaders",
                    .mCacheDirectory = command.mConfig.getCachePath(),
                    .mWidth = 1,
                    .mHeight = 1,
                    .mValidation = validation,
                },
                reason);
            if (renderer == nullptr)
            {
                out() << reason << '\n';
                return 1;
            }

            out() << renderer->describeDevice();
            return 0;
        }

        int runView(
            World& world, const std::string& cellSpec, const Rtx::ValidationOptions& validation, ViewRequest request)
        {
            const ESM::Cell* cell = findCellOrComplain(world.getContent(), cellSpec);
            if (cell == nullptr)
                return 1;

            printCellHeading(*cell);

            return runWindow(world, *cell, validation, std::move(request));
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

            /// Resolved here rather than left to each command, so a view measured at dawn under an
            /// overcast is drawn that way by every one of them.
            float mHour = sDefaultHour;
            std::string mWeather = std::string(sDefaultWeather);
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
                    .mHour = hourFor(hourGiven(variables), std::nullopt),
                    .mWeather = weatherFor(weatherGiven(variables), std::nullopt),
                };

            return Chosen{
                .mCell = view->mCell,
                .mTitle = "OpenMW RTX - " + view->mName,
                .mView = view->mName,
                .mNote = view->mNote,
                .mOrigin = origin.has_value() ? origin : view->mOrigin,
                .mTarget = target.has_value() ? target : view->mTarget,
                .mHour = hourFor(hourGiven(variables), view->mHour),
                .mWeather = weatherFor(weatherGiven(variables), view->mWeather),
            };
        }

        /// One stop, from a place the command line named. The day is the command line's alone: a
        /// view file entry names an hour and a weather and never a date.
        MWRender::Stop stopFrom(const Chosen& chosen, const FrameRequest& frame)
        {
            MWRender::Stop stop;
            stop.mName = chosen.mView.empty() ? chosen.mCell : chosen.mView;
            stop.mNote = chosen.mNote;
            stop.mCell = chosen.mCell;
            stop.mStand.mCell = chosen.mCell;
            stop.mStand.mEye = chosen.mOrigin;
            stop.mStand.mLook = chosen.mTarget;
            stop.mSky.mHour = chosen.mHour;
            stop.mSky.mDay = frame.mDay;
            stop.mSky.mWeather = chosen.mWeather;

            return stop;
        }

        /// One stop, from an entry in the view file.
        ///
        /// **The same fields `Chosen` carries, because a view is where a `Chosen` comes from.** A
        /// list of places and a single place are one kind of thing, so a run of six and a shot of
        /// one stand under the rules stated once.
        MWRender::Stop stopFrom(const View& view, const FrameRequest& frame)
        {
            // **`describeStaging` decides which of the two wins, and it is asked rather than
            // copied.** A view fixes the conditions its frame is about and the command line
            // overrules it, and a rule applied at one place and not the other would put a picture
            // and a number under different skies.
            const StagingRequest staging = frame.describeStaging(view);

            MWRender::Stop stop;
            stop.mName = view.mName;
            stop.mNote = view.mNote;
            stop.mCell = view.mCell;
            stop.mStand.mCell = view.mCell;
            stop.mStand.mEye = view.mOrigin;
            stop.mStand.mLook = view.mTarget;
            stop.mSky.mHour = staging.mHour;
            stop.mSky.mDay = staging.mDay;
            stop.mSky.mWeather = staging.mWeather;

            // **A route flies the player, which is what puts a cell arriving into a measurement.**
            // Where it ends is another view's camera, copied into the entry when the file was read.
            if (view.mRoute.has_value())
                stop.mSchedule.mRoute = MWRender::Route{
                    .mTo = view.mRoute->mOrigin,
                    .mLookTo = view.mRoute->mTarget,
                    .mSpeed = view.mRoute->mSpeed,
                };

            return stop;
        }

        /// How long every stop of a run lasts, from what the command line asked for.
        ///
        /// **Frames win over seconds where both were named.** `--frames` is what a run that has to
        /// be exactly reproducible asks for, and `--seconds` is what a run being read asks for.
        Rtx::BenchSpec specFrom(const bpo::variables_map& variables)
        {
            Rtx::BenchSpec spec;
            spec.mRun = variables["frames"].as<std::uint32_t>() > 0
                ? Rtx::BenchSpan{ .mFrames = variables["frames"].as<std::uint32_t>() }
                : Rtx::BenchSpan{ .mSeconds = variables["seconds"].as<float>() };
            spec.mWarm = Rtx::BenchSpan{ .mSeconds = variables["warmup"].as<float>() };

            return spec;
        }

        /// Everything a hosted run writes into the settings before the engine reads them: the
        /// window it is presented in, and the knobs the trace is made with.
        ///
        /// **These are settings and not a second command line**, because both binaries have to
        /// reach one renderer configured one way. The game used to hard-code every one of them and
        /// the harness used to take each as an option, so a picture taken here and a frame played
        /// were traced by two differently configured renderers.
        void applyHostedSettings(const bpo::variables_map& variables, const FrameRequest& frame)
        {
            const auto [width, height] = parseSize(variables["size"].as<std::string>());
            Settings::video().mResolutionX.set(static_cast<int>(width));
            Settings::video().mResolutionY.set(static_cast<int>(height));
            Settings::video().mWindowMode.set(Settings::WindowMode::Windowed);
            Settings::camera().mFieldOfView.set(frame.mFieldOfView);

            Settings::rtx().mUpscale.set(std::string(Rtx::upscaleName(frame.mUpscale)));
            Settings::rtx().mPreset.set(std::string(Rtx::presetName(frame.mPreset)));
            Settings::rtx().mReorder.set(std::string(Rtx::reorderName(frame.mReorder)));
            Settings::rtx().mDelight.set(frame.mDelight);
            Settings::rtx().mShowAlbedo.set(frame.mShowAlbedo);
            Settings::rtx().mFilter.set(frame.mFilter);

            // **Nought means measure it, which is what `--exposure=auto` says.** A setting has no
            // way to be absent, so the number the absence stands for is the one nothing multiplies.
            Settings::rtx().mExposure.set(frame.mExposure.value_or(0.0f));
            Settings::rtx().mJitter.set(frame.mJitter);
            Settings::rtx().mCountCrossings.set(frame.mCountCrossings);
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
                wanted = Rtx::splitNames(named);

            const std::vector<View> chosen = chooseViews(views, wanted);
            if (chosen.empty())
                throw std::runtime_error("nothing to profile: no view was named");

            return chosen;
        }

        int runListViews(const std::filesystem::path& resources)
        {
            for (const View& view : loadViews(resources / "rtx" / "views.cfg"))
            {
                out() << "  " << view.mName << "\n      " << view.mCell;

                // A place that fixes a condition is a different frame from the same camera at noon
                // under a clear sky, and this listing is how a view is found.
                if (view.mHour.has_value())
                    out() << " at " << Rtx::describeHour(*view.mHour);

                if (view.mWeather.has_value())
                    out() << " in " << *view.mWeather;

                out() << "\n      " << view.mNote << '\n';
            }

            return 0;
        }

        int commandInfo(const Command& command)
        {
            const Rtx::ValidationOptions validation = validationFrom(command.mVariables, false);

            return runInfo(command, validation);
        }

        int commandTextures(const Command& command)
        {
            const bpo::variables_map& variables = command.mVariables;
            const Chosen chosen = chooseView(variables, command.mResources);

            const Content content(command.mConfig, variables, command.mResources);
            World world(content);
            pageTerrainFrom(world, variables);

            const ESM::Cell* cell = findCellOrComplain(content, chosen.mCell);
            if (cell == nullptr)
                return 1;

            const FrameRequest frame = frameFrom(command, chosen.mHour, chosen.mWeather);

            return runTextures(world, *cell, frame.describeStaging(), frame.mActors, variables["out"].as<std::string>(),
                frame.mDelight);
        }

        int commandDoll(const Command& command)
        {
            const bpo::variables_map& variables = command.mVariables;

            // **The first of them, because `--npc` is the row `shot` stands up.** One picture is of
            // one person, and the option is shared rather than duplicated so that the same name
            // reaches the same record either way.
            //
            // Read before the content files are, so a run that named nobody says so in the time it
            // takes to print a line.
            const StringsVector people = variables["npc"].as<StringsVector>();
            if (people.empty())
            {
                out() << "doll needs somebody: --npc=<id>. `openmw-rtxtool scene --find=<text>` finds one.\n";
                return 1;
            }

            const PictureRequest request
                = pictureFrom(command, SceneUtil::sInventoryWidth, SceneUtil::sInventoryHeight);

            const Content content(command.mConfig, variables, command.mResources);
            World world(content);

            const ESM::NPC* npc = findNpc(content, people.front());
            if (npc == nullptr)
            {
                out() << "no NPC record is called \"" << people.front() << "\".\n";
                return 1;
            }

            return runDoll(world, *npc, validationFrom(variables, false), request);
        }

        int commandMap(const Command& command)
        {
            const bpo::variables_map& variables = command.mVariables;
            const Chosen chosen = chooseView(variables, command.mResources);
            const PictureRequest request = pictureFrom(command, 1024, 1024);

            const Content content(command.mConfig, variables, command.mResources);
            World world(content);
            pageTerrainFrom(world, variables);

            const ESM::Cell* cell = findCellOrComplain(content, chosen.mCell);
            if (cell == nullptr)
                return 1;

            const FrameRequest frame = frameFrom(command, chosen.mHour, chosen.mWeather);

            return runMap(
                world, *cell, frame.describeStaging(), frame.mActors, validationFrom(variables, false), request);
        }

        int commandScene(const Command& command)
        {
            const bpo::variables_map& variables = command.mVariables;
            const Chosen chosen = chooseView(variables, command.mResources);

            const Content content(command.mConfig, variables, command.mResources);
            World world(content);
            pageTerrainFrom(world, variables);

            const ESM::Cell* cell = findCellOrComplain(content, chosen.mCell);
            if (cell == nullptr)
                return 1;

            const std::string needle = variables["find"].as<std::string>();
            if (!needle.empty())
                return runFind(content, *cell, needle);

            const FrameRequest frame = frameFrom(command, chosen.mHour, chosen.mWeather);

            return runScene(world, *cell, frame.describeStaging(), frame.mActors, variables["twice"].as<bool>());
        }

        int commandVerify(const Command& command)
        {
            const bpo::variables_map& variables = command.mVariables;
            const FrameRequest frame
                = frameFrom(command, variables["hour"].as<float>(), variables["weather"].as<std::string>());

            std::vector<View> views = chooseViews(loadViews(command.mResources / "rtx" / "views.cfg"),
                Rtx::splitNames(variables["views"].as<std::string>()));
            applyConditions(variables, views);

            applyHostedSettings(variables, frame);

            // **Upscaling off, and not offered as an option.** Ray Reconstruction is temporal and
            // carries state nothing below can hold still: two builds that describe the same scene
            // identically write different bytes through it, and fifteen of sixteen views once read
            // as changed by a refactor that changed nothing.
            Settings::rtx().mUpscale.set(std::string(Rtx::upscaleName(Rtx::Upscale::Off)));

            const std::filesystem::path out
                = variables["out"].defaulted() ? "verify" : variables["out"].as<std::string>();
            std::filesystem::create_directories(out);

            // **Every view held still, because what this compares is the picture and not a run.**
            // A frame that animated between two builds would differ for a reason nobody is looking
            // for, and the whole point is that a refactor leaves the picture exactly as it was.
            MWRender::SessionRequest request;
            request.mStops.reserve(views.size());
            for (const View& view : views)
            {
                MWRender::Stop stop = stopFrom(view, frame);
                stop.mSchedule.mSpec.mWarm = Rtx::BenchSpan{ .mSeconds = variables["warmup"].as<float>() };
                stop.mSchedule.mSpec.mRun = Rtx::BenchSpan{ .mFrames = 1 };
                stop.mSchedule.mFrozen = true;
                stop.mActions.mCapture = out / (view.mName + ".png");
                request.mStops.push_back(std::move(stop));
            }

            request.mValidation = validationForMeasuring(variables, false);

            if (const int status = runHosted(variables, command.mConfig, command.mResources, std::move(request));
                status != 0)
                return status;

            return compareRuns(out, variables["against"].as<std::string>(), views);
        }

        int commandBench(const Command& command)
        {
            const bpo::variables_map& variables = command.mVariables;
            const FrameRequest frame
                = frameFrom(command, variables["hour"].as<float>(), variables["weather"].as<std::string>());

            std::string suite;
            std::vector<View> views = chooseBenchViews(variables, command.mResources, suite);
            applyConditions(variables, views);

            applyHostedSettings(variables, frame);

            const Rtx::BenchSpec spec = specFrom(variables);
            const std::vector<std::string> turn = Rtx::splitNames(variables["turn-weather"].as<std::string>());
            const bool hashing
                = !variables["hashes"].as<std::string>().empty() || !variables["against"].as<std::string>().empty();

            MWRender::SessionRequest request;
            request.mStops.reserve(views.size());
            for (const View& view : views)
            {
                MWRender::Stop stop = stopFrom(view, frame);
                stop.mSchedule.mSpec = spec;
                stop.mSky.mTurnThrough = turn;
                stop.mActions.mHash = hashing;
                request.mStops.push_back(std::move(stop));
            }

            request.mSuite = suite;
            request.mJson = variables["json"].as<std::string>();
            request.mHashes = variables["hashes"].as<std::string>();
            request.mAgainst = variables["against"].as<std::string>();
            request.mPerfControl = variables["perf-control"].as<std::string>();
            request.mHeadless = !variables["window"].as<bool>();
            request.mValidation = validationForMeasuring(variables, !request.mHeadless);

            return runHosted(variables, command.mConfig, command.mResources, std::move(request));
        }

        /// A screenshot of the game's own world, taken headless.
        ///
        /// **The world a player stands in and not one staged here**, which is the whole of what
        /// this path is for: the cells are read by `MWWorld::Scene`, the people are dressed by
        /// `NpcAnimation` and the sky is reported by `MWWorld::WeatherManager`, so the picture is
        /// the one the game draws rather than one derived beside it.
        int commandShot(const Command& command)
        {
            const bpo::variables_map& variables = command.mVariables;
            const Chosen chosen = chooseView(variables, command.mResources);
            const FrameRequest frame = frameFrom(command, chosen.mHour, chosen.mWeather);

            applyHostedSettings(variables, frame);

            MWRender::Stop stop = stopFrom(chosen, frame);

            // **Warmed and then held still, because a shot is a still.** The world has to arrive —
            // the ring read, the models built, the emitters run up — before the frame that is kept
            // means anything, and holding the clock after that is what makes two runs of one build
            // the same picture.
            stop.mSchedule.mSpec.mWarm = Rtx::BenchSpan{ .mSeconds = variables["warmup"].as<float>() };
            stop.mSchedule.mSpec.mRun
                = Rtx::BenchSpan{ .mFrames = std::max(variables["repeat"].as<std::uint32_t>(), 1u) };
            stop.mSchedule.mFrozen = true;
            stop.mActions.mCapture = variables["out"].as<std::string>();
            stop.mActions.mDump = variables["dump"].as<std::string>();
            stop.mActions.mTail = variables["tail"].as<bool>();
            stop.mSchedule.mAccumulate = variables["accumulate"].as<std::uint32_t>();

            // **Accumulating replaces repeating rather than joining it.** A run that also honoured
            // the repeat default would quietly average eight frames more than it was asked for, and
            // a convergence ladder built on that reads as though the first frames bought nothing.
            if (stop.mSchedule.mAccumulate > 0)
                stop.mSchedule.mSpec.mRun = Rtx::BenchSpan{ .mFrames = stop.mSchedule.mAccumulate };

            MWRender::SessionRequest request;
            request.mStops.push_back(std::move(stop));
            request.mValidation = validationFrom(variables, false);

            return runHosted(variables, command.mConfig, command.mResources, std::move(request));
        }

        /// A window on a cell, flown by hand. The one command that still stages a world of its
        /// own, until the game's is what it opens on.
        int commandView(const Command& command)
        {
            const bpo::variables_map& variables = command.mVariables;
            const Chosen chosen = chooseView(variables, command.mResources);

            const FrameRequest frame = frameFrom(command, chosen.mHour, chosen.mWeather);
            const Rtx::ValidationOptions validation = validationFrom(variables, true);

            const Content content(command.mConfig, variables, command.mResources);
            World world(content);
            pageTerrainFrom(world, variables);

            ViewRequest request;
            request.mFrame = frame;
            request.mTitle = chosen.mTitle;
            request.mView = chosen.mView;
            request.mNote = chosen.mNote;
            request.mCell = chosen.mCell;
            request.mScreenshotDirectory = command.mConfig.getScreenshotPath();
            request.mOrigin = chosen.mOrigin;
            request.mTarget = chosen.mTarget;
            request.mFrames = variables["frames"].as<std::uint32_t>();

            return runView(world, chosen.mCell, validation, request);
        }

        /// One verb: which command it is, the line `--help` prints for it, and what it does.
        ///
        /// **Which one it is and not what it is called**, because `verbs.hpp` holds the names: an
        /// option says which commands read it in the same terms this table names them in, so the
        /// two cannot drift into a command whose options nothing reaches.
        struct Verb
        {
            Verbs mVerb;
            std::string_view mSummary;
            int (*mRun)(const Command&);
        };

        /// Every command there is.
        ///
        /// **One list and not two.** The usage printed a name and a summary for each and the
        /// dispatch matched each name against a block of its own, so a verb added to one of them and
        /// forgotten in the other was either a command nobody could find or a line of help nothing
        /// answered. In the order `--help` prints them, which is the order they were written to be
        /// read in rather than a sorted one.
        constexpr std::array<Verb, 9> sVerbs{
            Verb{ Verbs::Info, "report the device this renderer would run on", commandInfo },
            Verb{ Verbs::Scene, "read a cell and report what the renderer would be handed", commandScene },
            Verb{ Verbs::Shot, "render a cell and write a PNG, with no window", commandShot },
            Verb{ Verbs::View, "open a window on a cell and fly around it", commandView },
            Verb{ Verbs::Bench, "time a run of frames at each of a list of places", commandBench },
            Verb{ Verbs::Textures, "every texture a cell uses, vanilla beside de-lit, as one sheet", commandTextures },
            Verb{ Verbs::Doll, "the inventory doll of one person, traced against a scene of its own", commandDoll },
            Verb{ Verbs::Map, "one local-map tile of a cell, traced straight down", commandMap },
            Verb{ Verbs::Verify, "render every view and say what moved since the last run", commandVerify },
        };

        void printUsage(const bpo::options_description& options)
        {
            out() << "Drives the experimental ray tracing renderer without the game window.\n\n"
                     "Usage: openmw-rtxtool <command> [options]\n\n"
                     "Commands:\n";

            for (const Verb& verb : sVerbs)
                out() << std::format("  {:<8} {}\n", verbName(verb.mVerb), verb.mSummary);

            out() << "\nWith no arguments at all: a window on the ship at Seyda Neen, where the game starts.\n\n"
                  << options;
        }

        int dispatch(int argc, char* argv[])
        {
            Platform::init();

            // The verb is taken straight off the command line rather than declared as a positional.
            // `ConfigurationManager::readConfiguration` walks the variables map and looks every key
            // up in the options description it was handed, so a key that is deliberately not in that
            // description — which is what a hidden positional is — makes it throw.
            //
            // A window is what this is for, so that is what it does when nobody says otherwise —
            // with no arguments at all, or with only options and no verb.
            const bool hasVerb = argc >= 2 && argv[1][0] != '-';
            const std::string_view command = hasVerb ? argv[1] : "view";

            const ToolOptions options = makeOptions(Rtx::sValidationByDefault);

            // Boost skips the first token as the program name; when there is a verb, that token is
            // the verb.
            //
            // **Held, because the line itself says what was asked for and the map does not.** A
            // variables map cannot tell an option somebody wrote from one `openmw.cfg` set or one
            // that came back defaulted, and what a command has to refuse is the first of the three.
            const bpo::parsed_options line = hasVerb
                ? bpo::command_line_parser(argc - 1, argv + 1).options(options.mDescription).run()
                : bpo::command_line_parser(argc, argv).options(options.mDescription).run();

            bpo::variables_map variables;
            bpo::store(line, variables);
            bpo::notify(variables);

            if (variables.count("help") > 0)
            {
                printUsage(options.mDescription);
                return 0;
            }

            Files::ConfigurationManager config;
            config.processPaths(variables, std::filesystem::current_path());
            config.readConfiguration(variables, options.mDescription);
            Debug::setupLogging(config.getLogPath(), applicationName);
            Settings::Manager::load(config);

            const std::filesystem::path resources = variables["resources"].as<Files::MaybeQuotedPath>();

            // Before the verb, as `--help` is: a switch that answers instead of the command is one
            // the command never sees.
            if (variables["list-views"].as<bool>())
                return runListViews(resources);

            const Verbs chosen = verbNamed(command);
            const auto found = std::find_if(
                sVerbs.begin(), sVerbs.end(), [chosen](const Verb& verb) { return verb.mVerb == chosen; });

            if (found == sVerbs.end())
            {
                out() << "Unknown command: " << command << "\n\n";
                printUsage(options.mDescription);
                return 1;
            }

            // **Before the command runs, because the alternative is a picture of somewhere else.**
            // Every option is declared on one description, so a command took every one of them and
            // read the ones it knew about: `shot --views=balmora` rendered the default view at
            // Seyda Neen and reported it without a word.
            if (const std::string complaint = options.complainAbout(line, chosen); !complaint.empty())
            {
                out() << complaint;
                return 1;
            }

            return found->mRun(Command{ variables, config, resources });
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
