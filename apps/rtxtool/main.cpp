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

#include <apps/openmw/mwrender/rtx/session.hpp>
#include <components/sceneutil/offscreenframing.hpp>
#include <components/settings/settings.hpp>
#include <components/settings/values.hpp>

#include "benchsuite.hpp"
#include "framerequest.hpp"
#include "hosted.hpp"
#include "options.hpp"
#include "parsefloat.hpp"
#include "validationchoice.hpp"
#include "verbs.hpp"
#include "verify.hpp"
#include "viewpoint.hpp"
#include "views.hpp"

namespace RtxTool
{
    namespace
    {
        namespace bpo = boost::program_options;

        using StringsVector = std::vector<std::string>;

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
            request.mDistantCells = variables["distant-cells"].as<float>();
            request.mDistantStatics = variables["distant-statics"].as<bool>();
            request.mExposure = parseExposure(variables["exposure"].as<std::string>());
            request.mWeather = weather;
            request.mHour = hour;
            request.mDay = variables["day"].as<int>();

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

        /// Where the command line and the view file meet, as one place.
        ///
        /// **A `View` and not a second type beside one.** A named view supplies the cell and usually
        /// the camera; anything given on the command line wins over it, so what comes back is the
        /// same kind of thing a suite hands out — and one function turns either into a stop.
        ///
        /// The conditions come back settled rather than optional, because they have been decided:
        /// `hourFor` and `weatherFor` are what decide them, and asking twice is how a picture and a
        /// number end up under different skies.
        View chooseView(const bpo::variables_map& variables, const std::filesystem::path& resources)
        {
            // Holds what the view below points into, for as long as this function needs it.
            std::vector<View> views;
            const View* view = findChosenView(variables, resources, views);

            // Anything given on the command line wins over the view, which is why the two optionals
            // are read first and the view is only consulted where they came back empty.
            const std::optional<osg::Vec3f> origin = parseVec3(variables["pos"].as<std::string>(), "--pos");
            const std::optional<osg::Vec3f> target = parseVec3(variables["look"].as<std::string>(), "--look");

            View place;
            place.mCell = view != nullptr ? view->mCell : variables["cell"].as<std::string>();
            place.mName = view != nullptr ? view->mName : std::string();
            place.mNote = view != nullptr ? view->mNote : std::string();
            place.mOrigin = origin.has_value() ? origin : (view != nullptr ? view->mOrigin : std::nullopt);
            place.mTarget = target.has_value() ? target : (view != nullptr ? view->mTarget : std::nullopt);
            place.mHour = hourFor(hourGiven(variables), view != nullptr ? view->mHour : std::nullopt);
            place.mWeather = weatherFor(weatherGiven(variables), view != nullptr ? view->mWeather : std::nullopt);
            place.mRoute = view != nullptr ? view->mRoute : std::nullopt;

            return place;
        }

        /// One stop, from a place a view file or a command line named.
        MWRender::Stop stopFrom(const View& place, const FrameRequest& frame)
        {
            // **`describeStaging` decides which of a view and a command line wins, and it is asked
            // rather than copied.** A rule applied at one place and not the other would put a
            // picture and a number under different skies.
            const StagingRequest staging = frame.describeStaging(place);

            MWRender::Stop stop;
            stop.mName = place.mName.empty() ? place.mCell : place.mName;
            stop.mNote = place.mNote;
            stop.mCell = place.mCell;
            stop.mStand.mCell = place.mCell;
            stop.mStand.mEye = place.mOrigin;
            stop.mStand.mLook = place.mTarget;
            stop.mSky.mHour = staging.mHour;
            stop.mSky.mDay = staging.mDay;
            stop.mSky.mWeather = staging.mWeather;

            // **A route flies the player, which is what puts a cell arriving into a measurement.**
            // Where it ends is another view's camera, copied into the entry when the file was read.
            if (place.mRoute.has_value())
                stop.mSchedule.mRoute = MWRender::Route{
                    .mTo = place.mRoute->mOrigin,
                    .mLookTo = place.mRoute->mTarget,
                    .mSpeed = place.mRoute->mSpeed,
                };

            return stop;
        }

        /// A stop that stands at one place, holds the world still and draws one frame of it.
        ///
        /// **What every command that writes a picture or a report wants.** The world has to arrive
        /// — the ring read, the models built, the emitters run up — before the frame that is kept
        /// means anything, and holding the clock after that is what makes two runs of one build the
        /// same picture.
        MWRender::Stop stillStopAt(const bpo::variables_map& variables, const View& place, const FrameRequest& frame)
        {
            MWRender::Stop stop = stopFrom(place, frame);
            stop.mSchedule.mSpec.mWarm = Rtx::BenchSpan{ .mSeconds = variables["warmup"].as<float>() };
            stop.mSchedule.mSpec.mRun = Rtx::BenchSpan{ .mFrames = 1 };
            stop.mSchedule.mFrozen = true;

            return stop;
        }

        /// Runs one stop against a real game, which is what every command that writes one picture
        /// or one report does.
        int runOneStop(const Command& command, MWRender::Stop stop)
        {
            MWRender::SessionRequest request;
            request.mStops.push_back(std::move(stop));
            request.mValidation = validationFrom(command.mVariables, false);

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
        /// reach one renderer configured one way. The game used to hard-code every one of them and
        /// the harness used to take each as an option, so a picture taken here and a frame played
        /// were traced by two differently configured renderers.
        void applyHostedSettings(const FrameRequest& frame)
        {
            Settings::video().mResolutionX.set(static_cast<int>(frame.mWidth));
            Settings::video().mResolutionY.set(static_cast<int>(frame.mHeight));
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
            Settings::rtx().mDistantLandCells.set(frame.mDistantCells);
            Settings::terrain().mObjectPaging.set(frame.mDistantStatics);
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

        /// Every texture the world around a place uses, vanilla beside de-lit, as one sheet.
        ///
        /// **Off the world the renderer is handed**, so the sheet holds what a frame of that place
        /// would actually sample — a town's people wear textures the town itself never names.
        int commandTextures(const Command& command)
        {
            const bpo::variables_map& variables = command.mVariables;
            const View place = chooseView(variables, command.mResources);
            const FrameRequest frame = frameFrom(command, *place.mHour, *place.mWeather);

            applyHostedSettings(frame);

            MWRender::Stop stop = stillStopAt(variables, place, frame);
            stop.mActions.mSheet = variables["out"].as<std::string>();

            return runOneStop(command, std::move(stop));
        }

        /// The inventory doll of one person, traced against a scene of its own.
        ///
        /// **The game's own preview and not a body assembled beside it.** `MWRender::NpcAnimation`
        /// dresses the parts a race calls for, equips what the record carries and finds the bone a
        /// weapon hangs on, and that is the path a played session takes.
        int commandDoll(const Command& command)
        {
            const bpo::variables_map& variables = command.mVariables;

            const StringsVector people = variables["npc"].as<StringsVector>();
            if (people.empty())
            {
                out() << "doll needs somebody: --npc=<id>. `openmw-rtxtool scene --find=<text>` finds one.\n";
                return 1;
            }

            const View place = chooseView(variables, command.mResources);
            const FrameRequest frame = frameFrom(command, *place.mHour, *place.mWeather);

            applyHostedSettings(frame);

            MWRender::Stop stop = stillStopAt(variables, place, frame);
            stop.mActions.mDoll = people.front();
            stop.mActions.mDollOut = variables["out"].as<std::string>();

            return runOneStop(command, std::move(stop));
        }

        /// One local-map tile of where a place stands, framed as the game's own compass frames one.
        int commandMap(const Command& command)
        {
            const bpo::variables_map& variables = command.mVariables;
            const View place = chooseView(variables, command.mResources);
            const FrameRequest frame = frameFrom(command, *place.mHour, *place.mWeather);

            applyHostedSettings(frame);

            MWRender::Stop stop = stillStopAt(variables, place, frame);
            stop.mActions.mMapTile = variables["out"].as<std::string>();

            return runOneStop(command, std::move(stop));
        }

        /// What the renderer was handed at a place, without looking at what it drew.
        int commandScene(const Command& command)
        {
            const bpo::variables_map& variables = command.mVariables;
            const View place = chooseView(variables, command.mResources);
            const FrameRequest frame = frameFrom(command, *place.mHour, *place.mWeather);

            applyHostedSettings(frame);

            MWRender::Stop stop = stillStopAt(variables, place, frame);
            stop.mActions.mFind = variables["find"].as<std::string>();
            stop.mActions.mDigest = stop.mActions.mFind.empty();
            stop.mActions.mWalkTwice = variables["twice"].as<bool>();

            return runOneStop(command, std::move(stop));
        }

        int commandVerify(const Command& command)
        {
            const bpo::variables_map& variables = command.mVariables;
            const FrameRequest frame
                = frameFrom(command, variables["hour"].as<float>(), variables["weather"].as<std::string>());

            std::vector<View> views = chooseViews(loadViews(command.mResources / "rtx" / "views.cfg"),
                Rtx::splitNames(variables["views"].as<std::string>()));
            applyConditions(variables, views);

            applyHostedSettings(frame);

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

            applyHostedSettings(frame);

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
            const View place = chooseView(variables, command.mResources);
            const FrameRequest frame = frameFrom(command, *place.mHour, *place.mWeather);

            applyHostedSettings(frame);

            // **Traced more than once, because one submit measures the clock and not the shader.**
            // This machine's GPU idles at 315 MHz and ramps only under load, so the same frame from
            // a cold start has timed anywhere between 0.37 and 2.1 ms.
            MWRender::Stop stop = stillStopAt(variables, place, frame);
            stop.mSchedule.mSpec.mRun
                = Rtx::BenchSpan{ .mFrames = std::max(variables["repeat"].as<std::uint32_t>(), 1u) };
            stop.mActions.mCapture = variables["out"].as<std::string>();
            stop.mActions.mDump = variables["dump"].as<std::string>();
            stop.mActions.mTail = variables["tail"].as<bool>();
            stop.mSchedule.mAccumulate = variables["accumulate"].as<std::uint32_t>();

            // **Accumulating replaces repeating rather than joining it.** A run that also honoured
            // the repeat default would quietly average eight frames more than it was asked for, and
            // a convergence ladder built on that reads as though the first frames bought nothing.
            if (stop.mSchedule.mAccumulate > 0)
                stop.mSchedule.mSpec.mRun = Rtx::BenchSpan{ .mFrames = stop.mSchedule.mAccumulate };

            return runOneStop(command, std::move(stop));
        }

        /// A window on a place, with the game running behind it.
        ///
        /// **The game and not a camera of this tool's own.** What a window is for is seeing how
        /// something moves and whether an artefact is a still or a shimmer, and both are questions
        /// about the frame a player gets — so the player is who flies it, with their own controls,
        /// their own collision and their own console.
        ///
        /// Collision comes off, because a view file's coordinates are where a camera stands rather
        /// than where a body fits.
        int commandView(const Command& command)
        {
            const bpo::variables_map& variables = command.mVariables;
            const View place = chooseView(variables, command.mResources);
            const FrameRequest frame = frameFrom(command, *place.mHour, *place.mWeather);

            applyHostedSettings(frame);

            MWRender::Stop stop = stopFrom(place, frame);

            // **A schedule with no end, because somebody is watching.** `--frames` closes it after
            // that many, which is how the window path gets exercised by something that cannot click.
            const std::uint32_t frames = variables["frames"].as<std::uint32_t>();
            stop.mSchedule.mSpec.mRun = Rtx::BenchSpan{ .mFrames = frames > 0 ? frames : sForever };
            stop.mSchedule.mFreeCamera = true;

            MWRender::SessionRequest request;
            request.mStops.push_back(std::move(stop));
            request.mHeadless = false;
            request.mQuitAtEnd = frames > 0;
            request.mValidation = validationFrom(variables, true);

            // What the place is called, for the block printed when the window closes. Where the eye
            // ends up is the game's to say.
            const Viewpoint spot{ .mView = place.mName, .mNote = place.mNote, .mCell = place.mCell };

            return runHosted(variables, command.mConfig, command.mResources, std::move(request), &spot);
        }

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
            const FrameRequest frame
                = frameFrom(command, variables["hour"].as<float>(), variables["weather"].as<std::string>());

            std::string suite;
            std::vector<View> views = chooseBenchViews(variables, command.mResources, suite);
            applyConditions(variables, views);

            applyHostedSettings(frame);

            const std::span<const MWRender::Check> every = MWRender::everyCheck();

            MWRender::SessionRequest request;
            request.mStops.reserve(views.size());
            for (const View& view : views)
            {
                MWRender::Stop stop = stopFrom(view, frame);
                stop.mSchedule.mSpec.mWarm = Rtx::BenchSpan{ .mSeconds = variables["warmup"].as<float>() };

                // **Two measured frames at least, because one of the claims is about a pair of
                // them.** A still camera resolving to a still picture cannot be asked of one frame.
                stop.mSchedule.mSpec.mRun = Rtx::BenchSpan{ .mFrames = 2 };
                stop.mSchedule.mFrozen = true;

                // **A route asks a claim the others cannot**, and a place that stands still cannot
                // answer it, so only a view that flies is asked about crossings.
                for (const MWRender::Check check : every)
                    if (check != MWRender::Check::CrossingsAppend || view.mRoute.has_value())
                        stop.mActions.mChecks.push_back(check);

                if (view.mRoute.has_value())
                {
                    stop.mSchedule.mFrozen = false;
                    stop.mSchedule.mSpec.mRun = Rtx::BenchSpan{ .mSeconds = variables["seconds"].as<float>() };
                }

                stop.mActions.mWalkTwice = true;
                request.mStops.push_back(std::move(stop));
            }

            request.mSuite = suite;
            request.mValidation = validationFrom(variables, false);

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
        constexpr std::array<Verb, 10> sVerbs{
            Verb{ Verbs::Info, "report the device this renderer would run on", commandInfo },
            Verb{ Verbs::Scene, "read a cell and report what the renderer would be handed", commandScene },
            Verb{ Verbs::Shot, "render a cell and write a PNG, with no window", commandShot },
            Verb{ Verbs::View, "open a window on a cell and fly around it", commandView },
            Verb{ Verbs::Bench, "time a run of frames at each of a list of places", commandBench },
            Verb{ Verbs::Textures, "every texture a cell uses, vanilla beside de-lit, as one sheet", commandTextures },
            Verb{ Verbs::Doll, "the inventory doll of one person, traced against a scene of its own", commandDoll },
            Verb{ Verbs::Map, "one local-map tile of a cell, traced straight down", commandMap },
            Verb{ Verbs::Verify, "render every view and say what moved since the last run", commandVerify },
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
