#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>
#include <osg/Vec3f>

#include <components/debug/debugging.hpp>
#include <components/files/configurationmanager.hpp>
#include <components/platform/platform.hpp>
#include <components/platform/process.hpp>
#include <components/rtx/error.hpp>
#include <components/rtx/reconstruction.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/upscale.hpp>
#include <components/rtxbench/benchrecord.hpp>
#include <components/rtxbench/benchrun.hpp>
#include <components/rtxbench/benchspec.hpp>
#include <components/rtxvulkan/createrenderer.hpp>
#include <components/sdlutil/vsyncmode.hpp>
#include <components/settings/settings.hpp>
#include <components/settings/values.hpp>
#include <components/settings/windowmode.hpp>

#include "compare.hpp"
#include "options.hpp"
#include "run.hpp"
#include "verbs.hpp"

namespace RtxTool
{
    namespace
    {
        namespace bpo = boost::program_options;

        constexpr std::string_view applicationName = "RtxTool";

        /// How long a window runs when nobody said: until it is closed. A count rather than a
        /// special case, so one schedule serves a run of eight frames and a session somebody flies
        /// for an hour.
        constexpr std::uint32_t sForever = ~0u;

        /// Which layers a run wants, from what the command line asked for.
        ///
        /// Shared because `info` and every other command have to agree: a device that reported its
        /// limits under one set of layers and traced under another would be describing something
        /// nobody ran.
        Rtx::ValidationOptions validationFrom(const bpo::variables_map& variables)
        {
            // A level somebody typed is demanded: a run that asked for the layers and cannot have
            // them fails naming what is missing, because an empty log reads as a clean pass, while
            // a build that turned them on by default only warns.
            return Rtx::ValidationOptions{
                .mLevel
                = Rtx::sValidationNames.require(variables["validation"].as<std::string>(), "a validation level"),
                .mDemanded = !variables["validation"].defaulted(),
            };
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

        /// What `--size` names.
        struct Size
        {
            std::uint32_t mWidth = 0;
            std::uint32_t mHeight = 0;
        };

        /// Parses `WIDTHxHEIGHT`.
        Size parseSize(std::string_view text)
        {
            const std::size_t cross = text.find('x');
            std::uint32_t width = 0;
            std::uint32_t height = 0;

            const bool ok = cross != std::string_view::npos
                && std::from_chars(text.data(), text.data() + cross, width).ec == std::errc()
                && std::from_chars(text.data() + cross + 1, text.data() + text.size(), height).ec == std::errc();

            if (!ok || width == 0 || height == 0)
                throw std::runtime_error("not a size: " + std::string(text));

            return Size{ .mWidth = width, .mHeight = height };
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

        /// The layers a run that will be measured or compared gets, which is none unless it asked.
        ///
        /// **Off unless somebody asked, whatever the build default is.** The layers cost between a
        /// tenth and half the frame rate, and a profiling run that quietly measured one under
        /// instrumentation is worse than no run at all: it produces a number, and the number is
        /// wrong. GPU-assisted validation instruments every shader besides, so a picture drawn under
        /// one is not the picture the next run will be compared against.
        Rtx::ValidationOptions validationForMeasuring(const bpo::variables_map& variables)
        {
            return variables["validation"].defaulted() ? Rtx::ValidationOptions{} : validationFrom(variables);
        }

        /// What `--hour` named, or nothing where it was left at its default. `placeFrom` is the rule
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

        /// Every view a run names, settled: a condition named on the command line is every
        /// place's, and none of them keeps its own.
        std::vector<Rtx::Stop> stopsFrom(
            const std::vector<Rtx::Stop>& views, const bpo::variables_map& variables, const FrameRequest& frame)
        {
            const std::optional<float> hour = hourGiven(variables);
            const std::optional<std::string> weather = weatherGiven(variables);

            std::vector<Rtx::Stop> stops;
            stops.reserve(views.size());
            for (const Rtx::Stop& view : views)
                stops.push_back(stopFor(view, hour, weather, frame.mDay));

            return stops;
        }

        /// What every command is handed: the line it was given, the configuration that line was
        /// read against, and where the resources are.
        struct Command
        {
            const bpo::variables_map& mVariables;
            Files::ConfigurationManager& mConfig;
            const std::filesystem::path& mResources;
            Verbs mVerb;
        };

        /// The whole of a `FrameRequest`, from the command line.
        ///
        /// @param hour,weather what the world stands under. `chooseView` decides them for a single
        ///        place, and `applyConditions` for a run of them — so they are passed rather than
        ///        read here.
        FrameRequest frameFrom(const Command& command)
        {
            const bpo::variables_map& variables = command.mVariables;
            const Size size = parseSize(variables["size"].as<std::string>());

            // **A window is the played game with the walls off, so what the player set stands
            // unless an option was typed over it.** Every other command has to state its frame,
            // so that two runs of it are one run whatever a settings file says — which is what
            // the harness's own defaults are for. A window that took them stood four cells of
            // ground under a player who had set eight, and upscaled at a quality they had not.
            const bool watched = command.mVerb == Verbs::View;
            const auto typed = [&](const char* name) { return !watched || !variables[name].defaulted(); };

            FrameRequest request;
            request.mWidth = size.mWidth;
            request.mHeight = size.mHeight;
            request.mFieldOfView = variables["fov"].as<float>();
            request.mDistantCells = typed("distant-cells") ? variables["distant-cells"].as<float>()
                                                           : Settings::rtx().mDistantLandCells.get();
            request.mDistantStatics = typed("distant-statics") ? variables["distant-statics"].as<bool>()
                                                               : Settings::terrain().mObjectPaging.get();
            request.mDay = variables["day"].as<int>();
            request.mVerticalSync = watched ? Settings::video().mVsyncMode.get() : SDLUtil::VSyncMode::Disabled;

            request.mProfile.mUpscaling.mMode = Rtx::sUpscaleNames.require(
                typed("upscale") ? variables["upscale"].as<std::string>() : Settings::rtx().mUpscale.get(),
                "an upscale mode");
            request.mProfile.mUpscaling.mPreset = Rtx::sPresetNames.require(
                typed("preset") ? variables["preset"].as<std::string>() : Settings::rtx().mPreset.get(),
                "a Ray Reconstruction preset");
            request.mProfile.mDelight = variables["delight"].as<float>();
            request.mProfile.mReconstruction.mFilter = variables["filter"].as<bool>();
            request.mProfile.mShowAlbedo = variables["albedo"].as<bool>();
            request.mProfile.mReconstruction.mJitter = variables["jitter"].as<bool>();
            request.mProfile.mExposure = parseExposure(variables["exposure"].as<std::string>());
            request.mProfile.mStressOverlapMs = variables["hold"].as<double>();

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
            try
            {
                const std::unique_ptr<Rtx::Renderer> renderer = Rtx::createVulkanRenderer(Rtx::RendererOptions{
                    .mShaderDirectory = command.mResources / "rtx" / "shaders",
                    .mCacheDirectory = command.mConfig.getCachePath(),
                    .mWidth = 1,
                    .mHeight = 1,
                    .mValidation = validation,
                });
                out() << renderer->describeDevice();
                return 0;
            }
            catch (const Rtx::Unsupported& obstacle)
            {
                out() << obstacle.what() << '\n';
                return 1;
            }
        }

        /// Where someone starts when they have said nothing about where: the ship at Seyda Neen,
        /// where the game starts and the one place every player of it has stood.
        constexpr std::string_view sDefaultView = "seyda-neen-ship";

        /// The view a run names, or null where it named none and gave a cell instead.
        ///
        /// Separated out because `Chosen` is built from it in one go below: an aggregate assembled
        /// in two stages cannot name every field in its initialiser, and the compiler is right to
        /// say so.
        /// Whether a run starts from a savegame, which is then what says where the player stands
        /// and what hour and weather it is — unless the line names a view or a cell over it.
        bool startsFromSave(const bpo::variables_map& variables)
        {
            return !variables["load-savegame"].as<Files::MaybeQuotedPath>().empty();
        }

        const Rtx::Stop* findChosenView(
            const bpo::variables_map& variables, const std::filesystem::path& resources, std::vector<Rtx::Stop>& views)
        {
            std::string name = variables["view"].as<std::string>();
            if (name.empty())
            {
                if (!variables["cell"].as<std::string>().empty() || startsFromSave(variables))
                    return nullptr;

                name = sDefaultView;
            }

            views = loadViews(resources / "rtx" / "views.cfg");
            const Rtx::Stop* view = findView(views, name);
            if (view != nullptr)
                return view;

            std::string known;
            for (const Rtx::Stop& candidate : views)
                known += "\n  " + candidate.mName + "   " + candidate.mNote;

            throw std::runtime_error("no view is called \"" + name + "\". These are:" + known);
        }

        /// The one place a command renders, and what a window would write it down as.
        /// Holds `stop` still: warmed as the command line asks, then `frames` measured with the
        /// simulation stopped.
        ///
        /// **Frozen is what still means.** The world does not step, so what one frame differs from
        /// the next by is the renderer and nothing else — which is what a picture, a digest and a
        /// pixel comparison are each about.
        ///
        /// @param frames how many to measure once the world has arrived. Why a command wants more
        ///        than one is that command's to say.
        void holdStill(Rtx::Stop& stop, const bpo::variables_map& variables, const std::uint32_t frames = 1)
        {
            stop.mSchedule.mSpec.mWarm = Rtx::BenchSpan{ .mSeconds = variables["warmup"].as<float>() };
            stop.mSchedule.mSpec.mRun = Rtx::BenchSpan{ .mFrames = frames };
            stop.mSchedule.mFrozen = true;
        }

        /// Runs a list of stops against a real game, which is what every command that writes
        /// pictures or reports does.
        int runStops(const Command& command, const Rtx::RenderProfile& profile, std::vector<Rtx::Stop> stops)
        {
            Rtx::SessionRequest request;
            request.mStops = std::move(stops);
            request.mSetup.mProfile = profile;
            request.mSetup.mValidation = validationFrom(command.mVariables);

            return runHosted(command.mVariables, command.mConfig, command.mResources, std::move(request));
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
        /// reach one engine configured one way. What the *trace* is configured by is
        /// `FrameRequest::mProfile`, which the renderer is handed directly.
        void applyHostedSettings(const FrameRequest& frame)
        {
            Settings::video().mResolutionX.set(static_cast<int>(frame.mWidth));
            Settings::video().mResolutionY.set(static_cast<int>(frame.mHeight));
            Settings::video().mWindowMode.set(Settings::WindowMode::Windowed);
            Settings::video().mVsyncMode.set(frame.mVerticalSync);
            Settings::camera().mFieldOfView.set(frame.mFieldOfView);

            // **What the engine reads and the renderer does not.** Everything the trace itself is
            // configured by travels as a `Rtx::RenderProfile` in the `RtxSetup` the renderer is made
            // with, so these are the settings a harness genuinely overrides rather than a channel
            // between two objects.
            Settings::rtx().mDistantLandCells.set(frame.mDistantCells);
            Settings::terrain().mObjectPaging.set(frame.mDistantStatics);
        }

        /// The one place a command names on its line, as a stop: a view, a cell, a save, and
        /// whatever the line says over them.
        ///
        /// **The frame is written into the settings before the stop is made**, so a picture and the
        /// sky it was framed for are one answer.
        Rtx::Stop stageOnePlace(const Command& command, const FrameRequest& frame)
        {
            const bpo::variables_map& variables = command.mVariables;

            applyHostedSettings(frame);

            // Holds what the view below points into, for as long as this function needs it.
            std::vector<Rtx::Stop> views;
            const Rtx::Stop* found = findChosenView(variables, command.mResources, views);
            const std::string cell = variables["cell"].as<std::string>();

            // **A save is the place, unless the line names one over it.** The stop then stands
            // where the save left the player, at the save's hour, day and weather, and only what
            // the line names is changed — where `stopFor` would stand it at noon under a clear sky
            // on the first day, which is a view's rule and not a save's.
            Rtx::Stop staged;
            if (found == nullptr && cell.empty() && startsFromSave(variables))
            {
                staged.mName = variables["load-savegame"].as<Files::MaybeQuotedPath>().stem().string();
                staged.mSky.mHour = hourGiven(variables);
                staged.mSky.mWeather = weatherGiven(variables);
                if (!variables["day"].defaulted())
                    staged.mSky.mDay = frame.mDay;
            }
            else
            {
                const Rtx::Stop view = found != nullptr ? *found : Rtx::Stop{ .mStand = { .mCell = cell } };
                staged = stopFor(view, hourGiven(variables), weatherGiven(variables), frame.mDay);
            }

            // Anything given on the command line wins over the view, which is the rule `stopFor`
            // already follows for the hour and the sky.
            if (const std::optional<osg::Vec3f> origin = parseVec3(variables["pos"].as<std::string>(), "--pos"))
                staged.mStand.mEye = origin;

            if (const std::optional<osg::Vec3f> target = parseVec3(variables["look"].as<std::string>(), "--look"))
                staged.mStand.mLook = target;

            return staged;
        }

        /// The places a picture or a report is made at, each held still: the views `--views` names,
        /// or the one place the line names where it names none.
        ///
        /// **Held still here and not by each command**, so a picture the world moved under cannot
        /// come out of a command that forgot the freeze, with nothing in the output to say so.
        ///
        /// @param frames how many to measure at each place once the world has arrived. Why a
        ///        command wants more than one is that command's to say.
        std::vector<Rtx::Stop> stagePlaces(
            const Command& command, const FrameRequest& frame, const std::uint32_t frames)
        {
            const bpo::variables_map& variables = command.mVariables;
            const std::string named = variables["views"].as<std::string>();

            std::vector<Rtx::Stop> stops;
            if (named.empty())
                stops.push_back(stageOnePlace(command, frame));
            else
            {
                applyHostedSettings(frame);
                stops = stopsFrom(
                    chooseViews(loadViews(command.mResources / "rtx" / "views.cfg"), Rtx::splitNames(named)), variables,
                    frame);
            }

            for (Rtx::Stop& stop : stops)
                holdStill(stop, variables, frames);

            return stops;
        }

        /// The places a run of a suite visits, in the order it visits them, with what the suite
        /// says about them.
        struct SuiteRun
        {
            std::vector<Rtx::Stop> mViews;

            /// Which suite, for the record's own header; empty where `--views` named the places.
            std::string mSuite;

            /// What the suite says about waiting for the ground, or nothing: `BenchSuite::mSettled`.
            std::optional<bool> mSettled;
        };

        /// **`--views` beats `--suite`, and both name entries in `views.cfg`.** A suite is a list
        /// written down so a run can be repeated without remembering it; a list on the command line
        /// is the same thing for one run. Neither carries coordinates: those live with the view, so
        /// the frame a picture is taken of and the frame a number is measured on stay one frame.
        ///
        /// @param ownSuite the suite the verb runs when neither `--views` nor `--suite` names one:
        ///        `bench` measures the frame budget's places and `check` walks a route as well.
        SuiteRun chooseBenchViews(const bpo::variables_map& variables, const std::filesystem::path& resources,
            const std::string_view ownSuite)
        {
            const std::vector<Rtx::Stop> views = loadViews(resources / "rtx" / "views.cfg");
            const std::string named = variables["views"].as<std::string>();

            SuiteRun run;
            std::vector<std::string> wanted;
            if (named.empty())
            {
                run.mSuite
                    = variables["suite"].defaulted() ? std::string(ownSuite) : variables["suite"].as<std::string>();

                const std::vector<BenchSuite> suites = loadSuites(resources / "rtx" / "benches.cfg");
                const BenchSuite* suite = findSuite(suites, run.mSuite);
                if (suite == nullptr)
                {
                    std::string known;
                    for (const BenchSuite& candidate : suites)
                        known += "\n  " + candidate.mName + "   " + candidate.mNote;

                    throw std::runtime_error("no suite is called \"" + run.mSuite + "\". These are:" + known);
                }

                wanted = suite->mViews;
                run.mSettled = suite->mSettled;
            }
            else
                wanted = Rtx::splitNames(named);

            run.mViews = chooseViews(views, wanted);
            if (run.mViews.empty())
                throw std::runtime_error("nothing to visit: no view was named");

            return run;
        }

        int runListViews(const std::filesystem::path& resources)
        {
            for (const Rtx::Stop& view : loadViews(resources / "rtx" / "views.cfg"))
            {
                out() << "  " << view.mName << "\n      " << view.mStand.mCell;

                // A place that fixes a condition is a different frame from the same camera at noon
                // under a clear sky, and this listing is how a view is found.
                if (view.mSky.mHour.has_value())
                    out() << " at " << Rtx::describeHour(*view.mSky.mHour);

                if (view.mSky.mWeather.has_value())
                    out() << " in " << *view.mSky.mWeather;

                out() << "\n      " << view.mNote << '\n';
            }

            return 0;
        }

        int commandInfo(const Command& command)
        {
            const Rtx::ValidationOptions validation = validationFrom(command.mVariables);

            return runInfo(command, validation);
        }

        /// What the renderer was handed at each place, without looking at what it drew.
        ///
        /// **Walked twice, always.** A second whole-graph walk should add nothing, and the report
        /// says what it added; a run that has read a cell is the cheapest place to ask.
        int commandScene(const Command& command)
        {
            const bpo::variables_map& variables = command.mVariables;
            const FrameRequest frame = frameFrom(command);

            std::vector<Rtx::Stop> stops = stagePlaces(command, frame, 1);
            for (Rtx::Stop& stop : stops)
            {
                stop.mActions.mFind = variables["find"].as<std::string>();
                stop.mActions.mDigest = stop.mActions.mFind.empty();
                stop.mActions.mWalkTwice = true;
            }

            return runStops(command, frame.mProfile, std::move(stops));
        }

        /// The pictures of each place, taken headless: the frame, and the doll, the tile and the
        /// sheet where asked for.
        ///
        /// **The world a player stands in and not one staged here**, which is the whole of what
        /// this path is for: the cells are read by `MWWorld::Scene`, the people are dressed by
        /// `NpcAnimation` and the sky is reported by `MWWorld::WeatherManager`, so the picture is
        /// the one the game draws rather than one derived beside it.
        ///
        /// **Every picture a stop can make, in one run**, because `Rtx::Actions` holds them all and
        /// each verb that made one started an engine of its own for it. And every view, under
        /// `--views`, with `--against` saying which pictures a change moved.
        int commandShot(const Command& command)
        {
            const bpo::variables_map& variables = command.mVariables;
            const FrameRequest frame = frameFrom(command);

            const std::filesystem::path against = variables["against"].as<std::string>();

            // **Traced more than once, because one submit measures the clock and not the shader.**
            // A GPU idles at a fraction of its clock and ramps only under load, so the same frame
            // from a cold start times anywhere within a factor of several.
            //
            // **Accumulating replaces repeating rather than joining it.** A run that also honoured
            // the repeat default would quietly average eight frames more than it was asked for, and
            // a convergence ladder built on that reads as though the first frames bought nothing.
            const std::uint32_t accumulate = variables["accumulate"].as<std::uint32_t>();
            const std::uint32_t frames
                = accumulate > 0 ? accumulate : std::max(variables["repeat"].as<std::uint32_t>(), 1u);

            std::vector<Rtx::Stop> stops = stagePlaces(command, frame, frames);

            const std::filesystem::path out
                = variables["out"].defaulted() ? "shot" : variables["out"].as<std::string>();
            std::filesystem::create_directories(out);

            const std::string doll = variables["doll"].as<std::string>();
            std::vector<std::string> written;
            for (Rtx::Stop& stop : stops)
            {
                stop.mSchedule.mAccumulate = accumulate;

                const auto file = [&](const std::string_view suffix) {
                    written.push_back(stop.mName + std::string(suffix) + ".png");
                    return out / written.back();
                };

                stop.mActions.mCapture = file("");
                if (!doll.empty())
                    stop.mActions.mDoll = Rtx::Actions::Doll{ .mWho = doll, .mFile = file("-doll") };
                if (variables["map"].as<bool>())
                    stop.mActions.mMapTile = file("-map");
                if (variables["textures"].as<bool>())
                    stop.mActions.mSheet = file("-textures");
            }

            if (const int status = runStops(command, frame.mProfile, std::move(stops)); status != 0)
                return status;

            return compareRuns(out, against, written);
        }

        int commandBench(const Command& command)
        {
            const bpo::variables_map& variables = command.mVariables;
            FrameRequest frame = frameFrom(command);

            // A bench draws frames the way a player sees them and sums none of them, so it is
            // measured at the width the game runs at. Every other verb keeps the reference's.
            frame.mProfile.mRadianceWidth = Rtx::RadianceWidth::Shown;

            applyHostedSettings(frame);

            const SuiteRun run = chooseBenchViews(variables, command.mResources, "default");
            std::vector<Rtx::Stop> stops = stopsFrom(run.mViews, variables, frame);

            const Rtx::BenchSpec spec = specFrom(variables);
            const std::vector<std::string> turn = Rtx::splitNames(variables["turn-weather"].as<std::string>());
            const bool hashing
                = !variables["hashes"].as<std::string>().empty() || !variables["against"].as<std::string>().empty();

            Rtx::SessionRequest request;
            request.mStops.reserve(stops.size());
            for (Rtx::Stop& stop : stops)
            {
                stop.mSchedule.mSpec = spec;
                stop.mSky.mTurnThrough = turn;
                stop.mActions.mHash = hashing;
                request.mStops.push_back(std::move(stop));
            }

            request.mSuite = run.mSuite;
            request.mJson = variables["json"].as<std::string>();
            request.mHashes = variables["hashes"].as<std::string>();
            request.mAgainst = variables["against"].as<std::string>();
            request.mPerfControl = variables["perf-control"].as<std::string>();
            request.mSetup.mProfile = frame.mProfile;
            request.mSetup.mSettled = run.mSettled;
            request.mSetup.mHeadless = !variables["window"].as<bool>();
            request.mSetup.mValidation = validationForMeasuring(variables);

            return runHosted(variables, command.mConfig, command.mResources, std::move(request));
        }

        /// A window on a place, with the game running behind it.
        ///
        /// **The game and not a camera of this tool's own.** What a window is for is seeing how
        /// something moves and whether an artefact is a still or a shimmer, and both are questions
        /// about the frame a player gets — so the player is who flies it, with their own controls,
        /// their own collision and their own console — in a body with every stat at 255, a Speed
        /// of 2000, level 255 and a million gold, and the frame rate on the window's title.
        ///
        /// Collision comes off, because a view file's coordinates are where a camera stands rather
        /// than where a body fits.
        int commandView(const Command& command)
        {
            const bpo::variables_map& variables = command.mVariables;
            const FrameRequest frame = frameFrom(command);

            Rtx::Stop staged = stageOnePlace(command, frame);

            // **A schedule with no end, because somebody is watching.** `--frames` closes it after
            // that many, which is how the window path gets exercised by something that cannot click.
            const std::uint32_t frames = variables["frames"].as<std::uint32_t>();
            staged.mSchedule.mSpec.mRun = Rtx::BenchSpan{ .mFrames = frames > 0 ? frames : sForever };
            staged.mSchedule.mFreeCamera = true;

            Rtx::SessionRequest request;
            request.mStops.push_back(std::move(staged));
            request.mQuitAtEnd = frames > 0;
            request.mSetup.mHeadless = false;
            request.mSetup.mValidation = validationFrom(variables);

            // **On the wall, because somebody is watching.** A stepped world runs as fast as the
            // card draws it, which at two hundred frames a second is three times over; a window
            // is the played game with the walls off, and the played game follows the wall.
            request.mSetup.mStep = std::nullopt;

            // Watched and never summed, like a bench.
            request.mSetup.mProfile = frame.mProfile;
            request.mSetup.mProfile.mRadianceWidth = Rtx::RadianceWidth::Shown;

            return runHosted(variables, command.mConfig, command.mResources, std::move(request), true);
        }

        /// Whether a place staged this way can answer `check` at all, which is a different question
        /// from whether it passes.
        ///
        /// **A claim a stop is not shaped for answers something else**, so it is left out rather
        /// than counted as a failure: a crossing count needs a route to cross anything with, and
        /// only the view says whether there is one.
        ///
        /// **Every check named, and no `default`**, so one added to `Rtx::Check` stops the
        /// build here and has to say which kind it is.
        bool canAsk(const Rtx::Check check, const Rtx::Stop& stop)
        {
            switch (check)
            {
                case Rtx::Check::CrossingsAppend:
                    return stop.mSchedule.mRoute.has_value();

                // **Asked of a stop that stands still.** A route leaves the camera wherever it
                // flew to and `Stand` names only where it set off from, so the two legitimately
                // differ by the whole length of the route.
                case Rtx::Check::CameraStands:
                    return stop.mStand.mEye.has_value() && !stop.mSchedule.mFreeCamera
                        && !stop.mSchedule.mRoute.has_value();

                // A route arrives at cells, and an arrival that rebuilds the scene, or places it
                // twice in one frame, drains the ring.
                case Rtx::Check::FramesOverlap:
                    return !stop.mSchedule.mRoute.has_value();

                case Rtx::Check::WalkTwice:
                case Rtx::Check::SurfacesDescribed:
                case Rtx::Check::LightsPlaced:
                case Rtx::Check::GroundReaches:
                case Rtx::Check::GroundStands:
                case Rtx::Check::LightsNotDoubled:
                case Rtx::Check::StaticsNotDoubled:
                case Rtx::Check::TexturesReadable:
                    return true;
            }

            return true;
        }

        /// How long `check` holds the queue after every frame's trace, in milliseconds, where the
        /// line names no `--hold` of its own.
        ///
        /// **Always, so a hazard that needs two frames in flight shows on the first frame of every
        /// run rather than on one run in four.** A held queue keeps the device that far behind the
        /// host, so every frame is recorded over a frame still running; `Rtx::StressPass` says
        /// what the hold is timed as. Eight is the hold the barrier gate ran under before it moved
        /// here, and half a frame at the target, so a place is not much slower for it.
        constexpr double sCheckHoldMs = 8.0;

        /// Every claim the tree makes about what the renderer is handed and what it draws, asked
        /// of a real game at each place of a suite.
        ///
        /// **These were tests against a world of this tool's own.** That world read its cells by
        /// hand, dressed its people by rules of its own and derived its sky from the content files,
        /// so a claim proved there was a claim about a world nobody plays. Asked here, each of them
        /// is about the world a player stands in.
        int commandCheck(const Command& command)
        {
            const bpo::variables_map& variables = command.mVariables;
            FrameRequest frame = frameFrom(command);
            if (variables["hold"].defaulted())
                frame.mProfile.mStressOverlapMs = sCheckHoldMs;

            applyHostedSettings(frame);

            const SuiteRun run = chooseBenchViews(variables, command.mResources, "check");
            std::vector<Rtx::Stop> stops = stopsFrom(run.mViews, variables, frame);

            const std::span<const Rtx::Check> every = Rtx::everyCheck();

            // **Every picture a stop can write, at the first place, because each leaves through a
            // path the frame's own passes never touch**: the capture adds the read back a picture
            // is written from, and the tile and the doll are offscreen traces with descriptor sets
            // and targets of their own. Under the layers, this is what finds a barrier they miss.
            const std::filesystem::path out
                = variables["out"].defaulted() ? "check" : variables["out"].as<std::string>();
            std::filesystem::create_directories(out);
            stops.front().mActions.mCapture = out / (stops.front().mName + ".png");
            stops.front().mActions.mMapTile = out / (stops.front().mName + "-map.png");
            stops.front().mActions.mDoll = Rtx::Actions::Doll{ "fargoth", out / (stops.front().mName + "-doll.png") };

            Rtx::SessionRequest request;
            request.mStops.reserve(stops.size());
            for (Rtx::Stop& stop : stops)
            {
                // **Two measured frames, because one of the claims is about a pair of them.** A
                // still camera resolving to a still picture cannot be asked of one frame.
                holdStill(stop, variables, 2);

                for (const Rtx::Check check : every)
                    if (canAsk(check, stop))
                        stop.mActions.mChecks.push_back(check);

                if (stop.mSchedule.mRoute.has_value())
                {
                    stop.mSchedule.mFrozen = false;
                    stop.mSchedule.mSpec.mRun = Rtx::BenchSpan{ .mSeconds = variables["seconds"].as<float>() };
                }

                stop.mActions.mWalkTwice = true;
                request.mStops.push_back(std::move(stop));
            }

            request.mSuite = run.mSuite;
            request.mSetup.mProfile = frame.mProfile;
            request.mSetup.mValidation = validationFrom(variables);

            return runHosted(variables, command.mConfig, command.mResources, std::move(request));
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
        constexpr std::array<Verb, 6> sVerbs{
            Verb{ Verbs::Info, "report the device this renderer would run on", commandInfo },
            Verb{ Verbs::Scene, "read a place and report what the renderer would be handed", commandScene },
            Verb{ Verbs::Shot,
                "the pictures of a place, with no window: the frame, a doll, a map tile, a texture sheet",
                commandShot },
            Verb{ Verbs::View, "open a window on a place and fly around it", commandView },
            Verb{ Verbs::Bench, "time a run of frames at each place of a suite", commandBench },
            Verb{ Verbs::Check, "assert what the renderer is handed and what it draws, at every place of a suite",
                commandCheck },
        };

        void printUsage(const bpo::options_description& options)
        {
            out() << "Drives the experimental ray tracing renderer without the game window.\n\n"
                     "Usage: openmw-rtxtool <command> [options]\n\n"
                     "Commands:\n";

            for (const Verb& verb : sVerbs)
                out() << std::format("  {:<8} {}\n", verbName(verb.mVerb), verb.mSummary);

            out() << "\nWith no arguments at all: a window on the ship at Seyda Neen, where the game starts.\n"
                     "The player's configuration is read and never written: what the engine saves on its way\n"
                     "out -- its settings, its log, its key bindings, its Lua storage -- goes to a directory of\n"
                     "this tool's own under the cache path.\n\n"
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

            const ToolOptions options
                = makeOptions(Rtx::sValidationByDefault ? Rtx::ValidationLevel::Sync : Rtx::ValidationLevel::Off);

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

            if (variables["help"].as<bool>())
            {
                printUsage(options.mDescription);
                return 0;
            }

            Files::ConfigurationManager config;

            // **Before the chain is walked, because this is the directory the engine writes into.**
            // `adoptConfigDirectory` says why a hosted run has one of its own; the log below lands
            // there too.
            adoptConfigDirectory(variables, ownConfigDirectory(config));

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

            return found->mRun(Command{ variables, config, resources, found->mVerb });
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
    // something whose whole point is to be run in a loop is the tool not working. `run` catches
    // its own exceptions, so the one box left is the crash catcher's, and upstream's own switch
    // turns that off — at the price of its crash log on a signal, which a debugger gives back.
    // Not overwritten, so that a shell can still ask for the catcher.
    Platform::Process::setEnvironmentDefault("OPENMW_DISABLE_CRASH_CATCHER", "1");

    return Debug::wrapApplication(RtxTool::run, argc, argv, RtxTool::applicationName);
}
