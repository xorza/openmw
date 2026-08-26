#include "view.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <memory>
#include <optional>
#include <ostream>

#include <SDL.h>

#include <components/debug/debugging.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/files/conversion.hpp>
#include <components/rtx/lightbuilder.hpp>
#include <components/rtx/png.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneuploader.hpp>
#include <components/rtx/shaders/visibility.h>

#include "framing.hpp"
#include "stagedworld.hpp"
#include "viewpoint.hpp"
#include "window.hpp"

namespace RtxTool
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        /// Two, so the CPU can prepare one frame while the GPU works on the other. More would add
        /// latency to a tool whose whole job is answering a mouse.
        constexpr std::uint32_t sFramesInFlight = 2;

        std::ostream& out()
        {
            return Debug::getRawStdout();
        }

        /// Runs its lambda however the scope is left.
        ///
        /// **The frame loop can throw, and what freed its semaphores and fences came after it.** A
        /// `checkVk` failure anywhere in a frame left all seven behind — and with the layers on by
        /// default that is worse than a leak: `vkDestroyDevice` reports them, the abort policy fires
        /// while the stack is still unwinding, and the error that actually happened never reaches
        /// anyone. What the loop broke on is what should be printed.
        template <typename Run>
        class OnScopeExit
        {
        public:
            explicit OnScopeExit(Run run)
                : mRun(std::move(run))
            {
            }

            OnScopeExit(const OnScopeExit&) = delete;
            OnScopeExit& operator=(const OnScopeExit&) = delete;

            ~OnScopeExit() { mRun(); }

        private:
            Run mRun;
        };

        /// Where the camera is standing now, under the conditions the window was opened with.
        Viewpoint spotOf(const ViewRequest& request, const FlyCamera& camera)
        {
            return Viewpoint{
                .mView = request.mView,
                .mNote = request.mNote,
                .mCell = request.mCell,
                .mOrigin = camera.getOrigin(),
                .mTarget = camera.getTarget(),
                .mWeather = request.mWeather,
                .mHour = request.mHour,
                .mDay = request.mDay,
            };
        }

        /// What a hand-over came to, for the line a ring prints.
        const char* describeUpload(const Rtx::SceneUpload& handed)
        {
            switch (handed.mKind)
            {
                case Rtx::SceneUpload::Kind::Placed:
                    return "nothing arrived, so only the transforms were rewritten";
                case Rtx::SceneUpload::Kind::Extended:
                    return "appended";
                case Rtx::SceneUpload::Kind::Rebuilt:
                    return "rebuilt, because the tables were renumbered";
            }

            return "handed over";
        }

        void printHelp()
        {
            out() << "\n"
                     "  W A S D        move,  Q E or ctrl/space for down and up\n"
                     "  right drag     look\n"
                     "  shift / alt    six times faster / seven times slower\n"
                     "  wheel          change the base speed\n"
                     "  T              run the clock,  a day and a half a minute\n"
                     "  , .            an hour back and forward,  shift for a day\n"
                     "  [ ]            the weather either side of this one, of those the region gets\n"
                     "  P              print this spot as a views.cfg block\n"
                     "  F3             print this spot as a command line, for profiling\n"
                     "  F2             write a screenshot\n"
                     "  F1             this list\n"
                     "  Esc            quit\n\n";
        }
    }

    int runWindow(World& world, const ESM::Cell& centre, const Rtx::ValidationOptions& validation, ViewRequest request,
        const ActorRequest& actors)
    {
        Window window(request.mTitle, request.mWidth, request.mHeight);

        std::string reason;
        const std::unique_ptr<Rtx::Renderer> renderer = Rtx::createRenderer(
            Rtx::RendererOptions{
                .mShaderDirectory = request.mShaderDirectory,
                .mWidth = request.mWidth,
                .mHeight = request.mHeight,
                .mUpscale = request.mUpscale,
                .mPreset = request.mPreset,
                .mWindow = window.getHandle(),
                .mValidation = validation,
            },
            reason);
        if (renderer == nullptr)
        {
            out() << reason << '\n';
            return 1;
        }

        // **The same staging the shot and the bench use, streaming included.** A window's camera
        // goes somewhere, which used to make it the one caller with its own copy of loading, the
        // ring, the sweep and the actor snapshot — and the copy is what drifted.
        StagedWorld staged(world, centre,
            StagingRequest{
                .mWeather = request.mWeather,
                .mHour = request.mHour,
                .mDay = request.mDay,
                .mFieldOfView = request.mFieldOfView,
                .mOrigin = request.mOrigin,
                .mTarget = request.mTarget,
            },
            actors);

        if (staged.empty())
        {
            out() << "Nothing to show: the region placed no geometry.\n";
            return 1;
        }

        request.mLighting = staged.getLighting();

        Rtx::SceneUploader uploader;

        /// Hands the renderer the scene as it now stands, building only what has to be built.
        ///
        /// **The same call the game makes, and it is what makes a ring cheap here too.** A crossing
        /// brings models the region did not have, which is a growth and not a renumbering, so the
        /// structures already built stay built and the textures already uploaded stay uploaded —
        /// a few milliseconds instead of the fifth of a second a full rebuild of the array costs.
        const auto hand = [&] {
            return uploader.hand(*renderer, Rtx::sWorld, staged.getScene(), world.getImageManager(), Rtx::SeaState{});
        };

        if (staged.getActorCount() > 0 || staged.getPropCount() > 0)
        {
            const Rtx::ExtractionStats& settled = staged.getSettled();
            out() << std::format(
                "{} actors and {} live props placed, {} deforming drawables, {} emitters holding "
                "{} particles\n",
                staged.getActorCount(), staged.getPropCount(), settled.mDeformed, settled.mEmitters, settled.mSprites);
        }

        const Placement start = staged.getPlacement();

        // **After everyone is in.** The bodies brought meshes of their own, so a build that ran
        // before them would leave the frame naming geometry it had no structure for.
        hand();

        FlyCamera camera;
        camera.look(start.mOrigin, start.mTarget);

        const float far = std::max(staged.getScene().getBounds().radius() * 8.0f, 10000.0f);

        printHelp();

        /// How fast the clock runs when it is running: game hours per real second.
        ///
        /// **A whole day in a minute and a half**, which is fast enough to watch a sunrise arrive
        /// and slow enough to see it happen. The game runs at a thirtieth of this; nothing here is
        /// pretending to be a play session.
        constexpr float sHoursPerSecond = 1.0f / 4.0f;

        /// How long a weather takes to become the next one, in real seconds.
        constexpr float sTransitionSeconds = 4.0f;

        bool clockRunning = false;

        /// The weather being turned into, and how far along. Empty where the sky is settled.
        std::optional<std::string> turningInto;
        float turned = 0.0f;

        /// Moves the sky to whatever the request now says, and takes the result back.
        ///
        /// **Nothing is reloaded.** The region, its lamps and its water are the same cell they were;
        /// only the arithmetic over the hour and the settings is done again, which is why a key can
        /// run the sun round the clock without a frame being dropped.
        const auto moveSky = [&] {
            if (turningInto.has_value())
                staged.setSky(request.mWeather, *turningInto, turned, request.mDay, request.mHour);
            else
                staged.setSky(request.mWeather, request.mDay, request.mHour);

            request.mLighting = staged.getLighting();
        };

        bool running = true;
        bool looking = false;
        bool resized = false;
        std::uint32_t drawn = 0;

        const auto handle = [&](const SDL_Event& event) {
            switch (event.type)
            {
                case SDL_QUIT:
                    running = false;
                    break;
                case SDL_WINDOWEVENT:
                    if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
                        resized = true;
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    if (event.button.button == SDL_BUTTON_RIGHT)
                    {
                        looking = true;
                        SDL_SetRelativeMouseMode(SDL_TRUE);
                    }
                    break;
                case SDL_MOUSEBUTTONUP:
                    if (event.button.button == SDL_BUTTON_RIGHT)
                    {
                        looking = false;
                        SDL_SetRelativeMouseMode(SDL_FALSE);
                    }
                    break;
                case SDL_MOUSEMOTION:
                    if (looking)
                        camera.turn(-event.motion.xrel * 0.0025f, -event.motion.yrel * 0.0025f);
                    break;
                case SDL_MOUSEWHEEL:
                    camera.scaleSpeed(event.wheel.y > 0 ? 1.3f : 1.0f / 1.3f);
                    break;
                case SDL_KEYDOWN:
                    if (event.key.keysym.sym == SDLK_ESCAPE)
                        running = false;
                    else if (event.key.keysym.sym == SDLK_F1)
                        printHelp();
                    else if (event.key.keysym.sym == SDLK_COMMA || event.key.keysym.sym == SDLK_PERIOD)
                    {
                        const bool forward = event.key.keysym.sym == SDLK_PERIOD;
                        const bool byDay = (event.key.keysym.mod & KMOD_SHIFT) != 0;

                        if (byDay)
                        {
                            // A day back from the first is a day before the world began, and the
                            // rise-hour formula counts from a fixed date rather than a signed one.
                            request.mDay = std::max(request.mDay + (forward ? 1 : -1), 0);
                        }
                        else
                        {
                            // Wrapped rather than clamped, so holding one of these walks the sun
                            // round and round instead of parking it at a horizon.
                            request.mHour = std::fmod(request.mHour + (forward ? 1.0f : 23.0f), 24.0f);
                        }

                        moveSky();
                    }
                    else if (event.key.keysym.sym == SDLK_LEFTBRACKET || event.key.keysym.sym == SDLK_RIGHTBRACKET)
                    {
                        const bool forward = event.key.keysym.sym == SDLK_RIGHTBRACKET;

                        // **Only the weathers the camera's own region ever gets.** Walking all ten
                        // offers skies the game would never produce there — snow on the Bitter Coast,
                        // an ashstorm on Solstheim — and a window is for looking at what the game
                        // looks like. The name cannot be one of the unknown ones by now: the region
                        // would have thrown while it was being lit.
                        // **Turned into rather than swapped for.** A transition is the one thing the
                        // harness never ran — the blend the shader carries was exercised only in the
                        // game, which is the surface nobody iterates on.
                        const std::uint32_t at = Rtx::weatherIndex(turningInto.value_or(request.mWeather)).value();

                        // Whatever the last one was turning into is where this one starts from, so
                        // pressing the key twice does not jump.
                        if (turningInto.has_value())
                            request.mWeather = *turningInto;

                        turningInto = std::string(Rtx::weatherName(
                            Rtx::nextRegionWeather(world.findRegion(staged.getRegion()), at, forward)));
                        turned = 0.0f;

                        moveSky();
                    }
                    else if (event.key.keysym.sym == SDLK_t)
                    {
                        clockRunning = !clockRunning;
                        out() << (clockRunning ? "the clock is running\n" : "the clock is stopped\n");
                    }
                    else if (event.key.keysym.sym == SDLK_p)
                    {
                        // The readable line above both formats, so a log of them says where each
                        // one is without anything having to parse it back first.
                        const Viewpoint spot = spotOf(request, camera);
                        out() << describeSpot(spot) << describeBlock(spot);
                    }
                    else if (event.key.keysym.sym == SDLK_F3)
                    {
                        const Rtx::FrameExtents shown = renderer->getExtents();
                        out() << describeSpot(spotOf(request, camera))
                              << describeProfile(request, validation, camera.getOrigin(), camera.getTarget(),
                                     shown.mOutputWidth, shown.mOutputHeight)
                              << '\n';
                    }
                    else if (event.key.keysym.sym == SDLK_F2 && drawn > 0)
                    {
                        const Rtx::FrameExtents shown = renderer->getExtents();
                        const std::filesystem::path file
                            = request.mScreenshotDirectory / ("rtx-" + std::to_string(SDL_GetTicks()) + ".png");
                        std::vector<std::uint8_t> pixels;
                        renderer->readPixels(pixels);
                        Rtx::writePng(file, shown.mOutputWidth, shown.mOutputHeight, pixels);
                        out() << "wrote " << Files::pathToUnicodeString(file) << '\n';
                    }
                    break;
                default:
                    break;
            }
        };
        Clock::time_point previous = Clock::now();
        const Clock::time_point began = previous;

        // Five times a second: fast enough that the coordinates keep up with the mouse, slow enough
        // that the compositor is not asked to redraw a title bar every frame.
        constexpr auto titleInterval = std::chrono::milliseconds(200);
        Clock::time_point lastTitle = previous;
        std::uint32_t framesSinceTitle = 0;

        while (running)
        {
            SDL_Event event;
            while (SDL_PollEvent(&event) != 0)
                handle(event);

            const Clock::time_point now = Clock::now();
            const float seconds = std::chrono::duration<float>(now - previous).count();
            previous = now;
            camera.advance(std::min(seconds, 0.1f));

            ++framesSinceTitle;
            if (now - lastTitle >= titleInterval)
            {
                const double elapsed = std::chrono::duration<double>(now - lastTitle).count();
                const osg::Vec3f at = camera.getOrigin();
                const Rtx::FrameExtents shown = renderer->getExtents();

                std::string sizes = std::format("{}x{}", shown.mOutputWidth, shown.mOutputHeight);
                if (shown.mRenderWidth != shown.mOutputWidth)
                    sizes = std::format("{}x{} to {}", shown.mRenderWidth, shown.mRenderHeight, sizes);

                window.setTitle(
                    std::format("{}  |  {:.0f} fps  |  {}  |  {:.0f}, {:.0f}, {:.0f}  |  {:.0f} u/s  |  day {} {} {}",
                        request.mTitle, framesSinceTitle / elapsed, sizes, at.x(), at.y(), at.z(), camera.getSpeed(),
                        request.mDay, clockFace(request.mHour),
                        turningInto.has_value()
                            ? std::format("{} to {} {:.0f}%", request.mWeather, *turningInto, turned * 100.0f)
                            : request.mWeather));

                framesSinceTitle = 0;
                lastTitle = now;
            }

            if (resized)
            {
                renderer->resize(window.getWidth(), window.getHeight());
                resized = false;
            }

            // **The region follows the camera.** Crossing out of the square the last ring was
            // centred on brings the one that is now in range and takes the cells behind it off the
            // graph, which is `StagedWorld`'s business and the bench's too.
            const Clock::time_point crossingStart = Clock::now();
            if (const Crossing crossed = staged.moveTo(camera.getOrigin()); crossed.happened())
            {
                // **What the ring cost, said out loud.** Appending is the whole point of taking the
                // game's decision here, and the line that says which branch ran is what turns a
                // claim about it into a measurement.
                const Rtx::SceneUpload handed = hand();
                out() << std::format("loaded {} cells and dropped {}, {} instances now placed — {} in {:.1f} ms\n",
                    crossed.mArrived, crossed.mDeparted, staged.getScene().getPlacedCount(), describeUpload(handed),
                    std::chrono::duration<double, std::milli>(Clock::now() - crossingStart).count());
            }

            // **The clock the world runs on, not the frame count.** A window that dropped frames
            // would otherwise animate in slow motion, and one that ran fast would gabble.
            //
            // Handed rather than placed, because stepping walks the whole graph and sweeps it: an
            // actor drawing a weapon brings a mesh nothing has built, and a sweep that closed a gap
            // renumbers what the last frame was built from.
            if (staged.advanceTo(static_cast<float>(std::chrono::duration<double>(now - began).count())))
                hand();

            // **The clock and whatever the weather is doing, both on real seconds.** A window is
            // the one surface with a wall clock, and these are what it is for: a sunrise that
            // arrives while you watch it, and the one transition between two weathers that nothing
            // else in this tool has ever run.
            if (clockRunning || turningInto.has_value())
            {
                if (clockRunning)
                    request.mHour = std::fmod(request.mHour + seconds * sHoursPerSecond, 24.0f);

                if (turningInto.has_value())
                {
                    turned += seconds / sTransitionSeconds;
                    if (turned >= 1.0f)
                    {
                        // Arrived: the sky it was turning into is simply the sky now.
                        request.mWeather = *turningInto;
                        turningInto.reset();
                        turned = 0.0f;
                    }
                }

                moveSky();
            }

            // The direction rather than `getTarget`, which exists so a person can read `look` in
            // `views.cfg` and tell where it points. Recovering it back out of two world points is
            // rounding, and a flying camera is where that shows.
            Framing framing;
            framing.mOrigin = camera.getOrigin();
            framing.mForward = camera.getForward();
            framing.mFieldOfView = request.mFieldOfView;
            framing.mFar = far;
            framing.mDelight = request.mDelight;
            framing.mShowAlbedo = request.mShowAlbedo;

            // The window is the only path with a clock. A screenshot leaves the sea still, so two
            // runs of one build agree pixel for pixel.
            framing.mLighting = request.mLighting;
            framing.mLighting.mSeconds = static_cast<float>(std::chrono::duration<double>(now - began).count());

            // What the fog's step jitter varies by, and what the upscaler's sample sequence is
            // walked by. A screenshot leaves it at zero and gets the same frame twice; here it has
            // to move, or twenty-four shells stand still in front of the camera and the jitter hides
            // nothing.
            framing.mFrame = drawn;

            renderer->renderFrame(makeFrameConstants(framing, renderer->getExtents()),
                Rtx::FrameOptions{ .mFilter = request.mFilter, .mExposure = request.mExposure });

            if (!renderer->presentFrame())
                resized = true;

            // Counted unconditionally: the summary at the end reports it whether or not a limit
            // was asked for, and `&&` would have skipped the increment in the interactive case.
            ++drawn;
            if (request.mFrames != 0 && drawn >= request.mFrames)
                running = false;
        }

        // One line at the end rather than one a second throughout: a number per second is noise to
        // someone watching the title bar, and scrollback to someone who ran this with --frames.
        const double lasted = std::chrono::duration<double>(Clock::now() - began).count();
        out() << std::format(
            "\n{} frames in {:.2f} s, {:.0f} fps average", drawn, lasted, drawn / std::max(lasted, 1e-6));

        // The same caveat `shot` prints beside its own figure: the layers are on by default outside
        // a Release build and cost about half the frame rate between them, so this is not a number
        // to compare against anything without `--validation=false`.
        if (renderer->isValidating())
            out() << ", with the validation layers on";

        out() << '\n';
        // Where it was left, so a session that ended somewhere worth keeping did not lose it.
        const Viewpoint spot = spotOf(request, camera);
        out() << describeSpot(spot) << describeBlock(spot);

        return 0;
    }
}
