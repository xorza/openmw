#include "view.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string_view>

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
#include <components/sky/clouds.hpp>
#include <components/sky/skyroll.hpp>

#include "content.hpp"
#include "framing.hpp"
#include "stagedworld.hpp"
#include "viewpoint.hpp"
#include "window.hpp"
#include "worldclock.hpp"

namespace RtxTool
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        std::ostream& out()
        {
            return Debug::getRawStdout();
        }

        /// Where the camera is standing now, under the conditions the window was opened with.
        Viewpoint spotOf(const ViewRequest& request, const FlyCamera& camera, const WorldClock& clock)
        {
            return Viewpoint{
                .mView = request.mView,
                .mNote = request.mNote,
                .mCell = request.mCell,
                .mOrigin = camera.getOrigin(),
                .mTarget = camera.getTarget(),
                .mWeather = request.mFrame.mWeather,
                .mHour = clock.getHour(),
                .mDay = clock.getDay(),
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

        /// One key the window answers, the line `F1` prints for it, and what it does.
        ///
        /// **One list and not two.** The help named every key and the event switch matched every
        /// key, so a binding added to one of them and forgotten in the other was either a key
        /// nobody could find or a line of help nothing answered.
        struct Binding
        {
            /// As `F1` prints it on the left, which is not always one key: `, .` is a pair that
            /// differ only in direction and reach one action.
            std::string_view mKeys;
            std::string_view mSummary;

            /// The keys it answers. `mSecond` is `SDLK_UNKNOWN` where there is only one.
            SDL_Keycode mFirst;
            SDL_Keycode mSecond;

            std::function<void(const SDL_Keysym&)> mPress;

            /// **`SDLK_UNKNOWN` answers nothing**, because SDL reports it for a key it has no name
            /// for, and every one-key binding would otherwise claim that press.
            bool answers(SDL_Keycode key) const { return key != SDLK_UNKNOWN && (key == mFirst || key == mSecond); }
        };

        void printHelp(std::span<const Binding> bindings)
        {
            // **Prose, because there is no binding to read these off.** `FlyCamera` asks SDL what is
            // held down every frame rather than answering an event, so these four describe input
            // nothing in the switch can drift from.
            out() << "\n"
                     "  W A S D        move,  Q E or ctrl/space for down and up\n"
                     "  right drag     look\n"
                     "  shift / alt    six times faster / seven times slower\n"
                     "  wheel          change the base speed\n";

            for (const Binding& binding : bindings)
                out() << std::format("  {:<14} {}\n", binding.mKeys, binding.mSummary);

            out() << '\n';
        }
    }

    int runWindow(World& world, const ESM::Cell& centre, const Rtx::ValidationOptions& validation, ViewRequest request)
    {
        Window window(request.mTitle, request.mFrame.mWidth, request.mFrame.mHeight);

        std::string reason;
        const std::unique_ptr<Rtx::Renderer> renderer
            = Rtx::createRenderer(request.mFrame.describeRenderer(validation, window.getHandle()), reason);
        if (renderer == nullptr)
        {
            out() << reason << '\n';
            return 1;
        }

        // **The same staging the shot and the bench use, streaming included.** A window's camera
        // goes somewhere, which used to make it the one caller with its own copy of loading, the
        // ring, the sweep and the actor snapshot — and the copy is what drifted.
        StagedWorld staged(
            world, centre, request.mFrame.describeStaging(request.mOrigin, request.mTarget), request.mFrame.mActors);

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

        /// How long a weather takes to become the next one, in real seconds.
        constexpr float sTransitionSeconds = 4.0f;

        /// The one clock the hour, the sea and the fog run on. `WorldClock` says which reading each
        /// thing takes and why.
        WorldClock clock(request.mFrame.mDay, request.mFrame.mHour);

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
                staged.setSky(
                    SkyMoment{ request.mFrame.mWeather, clock.getDay(), clock.getHour() }, *turningInto, turned);
            else
                staged.setSky(SkyMoment{ request.mFrame.mWeather, clock.getDay(), clock.getHour() });

            request.mLighting = staged.getLighting();
        };

        bool running = true;
        bool looking = false;
        bool resized = false;
        std::uint32_t drawn = 0;

        const std::array<Binding, 8> bindings{
            Binding{ "T", "run the clock,  a day and a half a minute,  and the air with it", SDLK_t, SDLK_UNKNOWN,
                [&](const SDL_Keysym&) {
                    clock.toggle();
                    out() << (clock.isRunning() ? "the clock is running\n" : "the clock is stopped\n");
                } },

            Binding{ ", .", "an hour back and forward,  shift for a day", SDLK_COMMA, SDLK_PERIOD,
                [&](const SDL_Keysym& pressed) {
                    const bool forward = pressed.sym == SDLK_PERIOD;

                    if ((pressed.mod & KMOD_SHIFT) != 0)
                        clock.nudgeDay(forward ? 1 : -1);
                    else
                        clock.nudgeHour(forward ? 1.0f : -1.0f);

                    moveSky();
                } },

            Binding{ "[ ]", "the weather either side of this one, of those the region gets", SDLK_LEFTBRACKET,
                SDLK_RIGHTBRACKET,
                [&](const SDL_Keysym& pressed) {
                    const bool forward = pressed.sym == SDLK_RIGHTBRACKET;

                    // **Only the weathers the camera's own region ever gets.** Walking all ten
                    // offers skies the game would never produce there — snow on the Bitter Coast,
                    // an ashstorm on Solstheim — and a window is for looking at what the game looks
                    // like. The name cannot be one of the unknown ones by now: the region would
                    // have thrown while it was being lit.
                    //
                    // **Turned into rather than swapped for.** A transition is the one thing the
                    // harness never ran — the blend the shader carries was exercised only in the
                    // game, which is the surface nobody iterates on.
                    const std::uint32_t at = Rtx::weatherIndex(turningInto.value_or(request.mFrame.mWeather)).value();

                    // Whatever the last one was turning into is where this one starts from, so
                    // pressing the key twice does not jump.
                    if (turningInto.has_value())
                        request.mFrame.mWeather = *turningInto;

                    turningInto = std::string(Rtx::weatherName(
                        Rtx::nextRegionWeather(world.getContent().findRegion(staged.getRegion()), at, forward)));
                    turned = 0.0f;

                    moveSky();
                } },

            Binding{ "P", "print this spot as a views.cfg block", SDLK_p, SDLK_UNKNOWN,
                [&](const SDL_Keysym&) {
                    // The readable line above both formats, so a log of them says where each one is
                    // without anything having to parse it back first.
                    const Viewpoint spot = spotOf(request, camera, clock);
                    out() << describeSpot(spot) << describeBlock(spot);
                } },

            Binding{ "F3", "print this spot as a command line, for profiling", SDLK_F3, SDLK_UNKNOWN,
                [&](const SDL_Keysym&) {
                    const Rtx::FrameExtents shown = renderer->getExtents();
                    out() << describeSpot(spotOf(request, camera, clock))
                          << describeProfile(request, validation, camera.getOrigin(), camera.getTarget(),
                                 shown.mOutputWidth, shown.mOutputHeight)
                          << '\n';
                } },

            Binding{ "F2", "write a screenshot", SDLK_F2, SDLK_UNKNOWN,
                [&](const SDL_Keysym&) {
                    if (drawn == 0)
                        return;

                    const Rtx::FrameExtents shown = renderer->getExtents();
                    const std::filesystem::path file
                        = request.mScreenshotDirectory / ("rtx-" + std::to_string(SDL_GetTicks()) + ".png");
                    std::vector<std::uint8_t> pixels;
                    renderer->readPixels(pixels);
                    Rtx::writePng(file, shown.mOutputWidth, shown.mOutputHeight, pixels);
                    out() << "wrote " << Files::pathToUnicodeString(file) << '\n';
                } },

            // The list holding the binding that prints it: the reference is bound while the array is
            // built and read only once it stands, which is every press.
            Binding{ "F1", "this list", SDLK_F1, SDLK_UNKNOWN, [&](const SDL_Keysym&) { printHelp(bindings); } },

            Binding{ "Esc", "quit", SDLK_ESCAPE, SDLK_UNKNOWN, [&](const SDL_Keysym&) { running = false; } },
        };

        printHelp(bindings);

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
                {
                    const auto found = std::find_if(bindings.begin(), bindings.end(),
                        [&](const Binding& binding) { return binding.answers(event.key.keysym.sym); });

                    if (found != bindings.end())
                        found->mPress(event.key.keysym);

                    break;
                }
                default:
                    break;
            }
        };

        Clock::time_point previous = Clock::now();
        const Clock::time_point began = previous;

        /// How far the deck has scrolled and the star sphere has turned.
        ///
        /// **Accumulated rather than taken off the clock, because the deck's speed moves under
        /// it.** A transition mixes one weather's `Cloud_Speed` into the next over four seconds,
        /// and a scroll worked out as elapsed time times the speed of the moment would drag the
        /// whole history along with it — a deck that lurches, and runs backwards where the weather
        /// ahead is the stiller one.
        Sky::SkyRoll skyRoll;

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
            clock.advance(std::chrono::duration<float>(now - previous).count());
            previous = now;
            camera.advance(clock.getStep());

            // **The sky's own two clocks, and neither of them is the hour.** A deck scrolls at the
            // speed its weather records whether or not the world's clock is running, and the stars
            // come round once every four *game* days — so the sphere turns only while the clock
            // key has the hour moving. `RenderingManager` turns the same roll for the game.
            skyRoll.advance(
                clock.getStep(), request.mLighting.mCloudSpeed, clock.getTimeScale(), Sky::timescaleClouds());

            ++framesSinceTitle;
            if (now - lastTitle >= titleInterval)
            {
                const double elapsed = std::chrono::duration<double>(now - lastTitle).count();
                const Rtx::FrameExtents shown = renderer->getExtents();

                window.setTitle(describeTitle(WindowTitle{
                    .mName = request.mTitle,
                    .mFps = framesSinceTitle / elapsed,
                    .mOutputWidth = shown.mOutputWidth,
                    .mOutputHeight = shown.mOutputHeight,
                    .mRenderWidth = shown.mRenderWidth,
                    .mRenderHeight = shown.mRenderHeight,
                    .mOrigin = camera.getOrigin(),
                    .mSpeed = camera.getSpeed(),
                    .mDay = clock.getDay(),
                    .mHour = clock.getHour(),
                    .mWeather = request.mFrame.mWeather,
                    .mInto = turningInto.has_value() ? std::string_view(*turningInto) : std::string_view(),
                    .mTurned = turned,
                }));

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
            if (staged.advanceTo(clock.getWallSeconds()))
                hand();

            // **The hour and whatever the weather is doing, and a window is the one surface with a
            // clock to run them on**: a sunrise that arrives while you watch it, and the one
            // transition between two weathers that nothing else in this tool has ever run. The
            // clock moved the hour above; the transition walks on the wall, because four seconds
            // of weather arriving is four seconds however fast the day goes.
            if (clock.isRunning() || turningInto.has_value())
            {
                if (turningInto.has_value())
                {
                    turned += clock.getStep() / sTransitionSeconds;
                    if (turned >= 1.0f)
                    {
                        // Arrived: the sky it was turning into is simply the sky now.
                        request.mFrame.mWeather = *turningInto;
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
            framing.mFieldOfView = request.mFrame.mFieldOfView;
            framing.mDelight = request.mFrame.mDelight;
            framing.mShowAlbedo = request.mFrame.mShowAlbedo;

            // **A screenshot is the path with no clock at all.** The seconds carry the sea and the
            // fog and the roll carries the sky, and a `shot` leaves both standing — which is what
            // makes two runs of one build agree pixel for pixel.
            //
            // **The world's seconds and not the wall's**, so that the air runs with the clock key.
            // The actors above keep the wall: a walk at thirty times is not a walk.
            framing.mLighting = request.mLighting;
            framing.mLighting.mSeconds = clock.getWorldSeconds();
            framing.mLighting.mRoll = skyRoll;

            // What the fog's step jitter varies by, and what the upscaler's sample sequence is
            // walked by. A screenshot leaves it at zero and gets the same frame twice; here it has
            // to move, or twenty-four shells stand still in front of the camera and the jitter hides
            // nothing.
            framing.mFrame = drawn;

            // **No `finishFrame`, and that is the point of this window.** A wait anywhere in this
            // loop caps the ring at one frame behind, the way `bench` and the game are; leaving it
            // out lets the ring fill, which is the only place in the tree where two frames are
            // really in flight. What that path gets wrong shows here and nowhere else. The numbers
            // it drops are numbers a window does not report.
            renderer->renderFrame(makeFrameConstants(framing, renderer->getExtents()),
                Rtx::FrameOptions{ .mExposureBias = framing.mLighting.mDaylight.mExposureBias,
                    .mFilter = request.mFrame.mFilter,
                    .mExposure = request.mFrame.mExposure });

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
        // The wall and not the clock's own reading of it, because this measures the tool and not
        // the world: a stall the clock stopped for is still time this run took.
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
        const Viewpoint spot = spotOf(request, camera, clock);
        out() << describeSpot(spot) << describeBlock(spot);

        return 0;
    }
}
