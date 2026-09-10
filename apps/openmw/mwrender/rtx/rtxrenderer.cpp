#include "rtxrenderer.hpp"

#include "setup.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <format>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <MyGUI_ITexture.h>
#include <MyGUI_RenderManager.h>
#include <SDL.h>
#include <osg/Camera>
#include <osg/FrameStamp>
#include <osg/Image>
#include <osg/Matrixf>
#include <osg/Node>
#include <osg/Stats>
#include <osg/Timer>
#include <osgGA/EventQueue>

#include <components/debug/debuglog.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/myguiplatform/myguiplatform.hpp>
#include <components/myguirtx/rendermanager.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/rtx/camera.hpp>
#include <components/rtx/error.hpp>
#include <components/rtx/frameclock.hpp>
#include <components/rtx/frameimage.hpp>
#include <components/rtx/frameworld.hpp>
#include <components/rtx/moonbuilder.hpp>
#include <components/rtx/namedenum.hpp>
#include <components/rtx/poseupdate.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/sceneuploader.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/upscale.hpp>
#include <components/rtxbench/frametimes.hpp>
#include <components/sceneutil/screencapture.hpp>
#include <components/sceneutil/vismask.hpp>
#include <components/sdlutil/imagetosurface.hpp>
#include <components/settings/values.hpp>
#include <components/surface/material.hpp>
#include <components/terrain/chunkmanager.hpp>

#include "../../mwbase/environment.hpp"
#include "../../mwbase/world.hpp"

#include "../camera.hpp"
#include "../offscreenview.hpp"
#include "../renderingmanager.hpp"
#include "../sceneframe.hpp"
#include "../screenshotwriter.hpp"
#include "../stage.hpp"
#include "../windowsetup.hpp"
#include "readworld.hpp"
#include "session.hpp"
#include "tracedview.hpp"
#include "worldmirror.hpp"

namespace MWRender
{
    namespace
    {
        /// What `[RTX]` says the trace is configured by, which is what a played binary runs.
        ///
        /// **Here and not in `components/rtx`**, because the settings registry is a global the core
        /// has no other reason to read: a harness hands its profile over in `RendererSpec::mRtx`.
        Rtx::RenderProfile profileFromSettings()
        {
            Rtx::RenderProfile profile;

            profile.mUpscale = Rtx::sUpscaleNames.require(Settings::rtx().mUpscale.get(), "an upscale mode");
            profile.mPreset = Rtx::sPresetNames.require(Settings::rtx().mPreset.get(), "a Ray Reconstruction preset");
            profile.mCountCrossings = Settings::rtx().mCountCrossings;
            profile.mDelight = Settings::rtx().mDelight;
            profile.mShowAlbedo = Settings::rtx().mShowAlbedo;
            profile.mFilter = Settings::rtx().mFilter;
            profile.mJitter = Settings::rtx().mJitter;

            // Nought is how a settings file says "measure it", there being no way to write nothing.
            if (const float exposure = Settings::rtx().mExposure; exposure > 0.0f)
                profile.mExposure = exposure;

            return profile;
        }

        /// A quarter of a Morrowind foot. Nothing is clipped against it — see `mNear` — so it only
        /// has to be nearer than anything the eye can find itself inside of.
        constexpr float sNear = 1.0f;

        /// How long the window must report one size before the renderer is rebuilt for it.
        ///
        /// **Because rebuilding costs about as long as this waits.** A new extent releases every
        /// target, allocates them again and uploads Ray Reconstruction's weights for the pair of
        /// resolutions it is now between — measured at a tenth of a second apiece over sixty
        /// rebuilds. A window dragged across a screen passes through hundreds of extents, and
        /// following each of them would draw the drag at ten frames a second.
        ///
        /// So a gesture is followed once it stops. Until then the surface keeps the extent it has,
        /// and what the compositor shows is that picture scaled — which is what a window being
        /// dragged shows anyway.
        ///
        /// **Six frames at sixty.** Long enough that a drag settles into one rebuild, short enough
        /// that letting go of a window edge and seeing the picture follow reads as immediate.
        constexpr double sSettleSeconds = 0.1;

        /// Whether an environment variable is set to anything other than nothing or `0`.
        bool askedFor(const char* name)
        {
            const char* const value = std::getenv(name);
            return value != nullptr && *value != '\0' && std::strcmp(value, "0") != 0;
        }

    }

    RtxRenderer::RtxRenderer(const RendererSpec& spec)
        : mStage(spec.mStage)
        , mCapture(makeScreenshotWriter(spec.mWorkQueue, spec.mScreenshotPath))
        , mUpdateVisitor(new Rtx::PoseUpdate)
        , mStartTick(osg::Timer::instance()->tick())
    {
        // **Made here and handed straight over, because the stage is where they live.** Every
        // renderer needs the four and one built on `osgViewer` gets them already wired together, so
        // the one that owns its own surface builds them and the stage holds them for both.
        const osg::ref_ptr<osg::Camera> camera = new osg::Camera;
        const osg::ref_ptr<osg::FrameStamp> frameStamp = new osg::FrameStamp;
        const osg::ref_ptr<osgGA::EventQueue> events = new osgGA::EventQueue;
        const osg::ref_ptr<osg::Stats> stats = new osg::Stats("Viewer");

        frameStamp->setFrameNumber(0);
        frameStamp->setReferenceTime(0.0);
        frameStamp->setSimulationTime(0.0);
        mUpdateVisitor->setFrameStamp(frameStamp);

        mStage.adopt(*camera, *frameStamp, *events, *stats);

        // **Read before anything is built, because it decides how the window opens and what the
        // trace counts.** A harness hands a whole run over in the spec; a played binary can only
        // name one in its settings, and a session that asked for neither behaves exactly as it did.
        mProfile
            = spec.mRtx != nullptr && spec.mRtx->mProfile.has_value() ? *spec.mRtx->mProfile : profileFromSettings();

        if (spec.mRtx != nullptr && spec.mRtx->mSession.has_value())
            mSession = std::make_unique<Session>(*spec.mRtx->mSession, spec.mRtx->mInto);
        else if (std::optional<Rtx::SessionRequest> setting = readSessionSetting())
            mSession = std::make_unique<Session>(std::move(*setting), nullptr);

        // **Before any content is read, because it decides what reading one records.** This is the
        // only renderer that asks what the content says a surface is, and the answer is stored on
        // every state set as it is built — so nothing else in the process pays for it.
        Surface::describeSurfaces(true);

        // **One name with the harness, because neither host has a GL context to ask.**
        mMaxTextureUnits = Surface::sAssumedTextureUnits;

        createWindow(spec.mResourceDir, mSession != nullptr && mSession->isHeadless());

        const Rtx::Upscale upscale = mProfile.mUpscale;
        const Rtx::Preset preset = mProfile.mPreset;

        // The window's own size, which `fitToWindow` asks for again on every frame after this one.
        // Kept, so that the first of those sees a size that has already settled.
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(mWindow, &width, &height);
        mAskedWidth = static_cast<std::uint32_t>(std::max(width, 1));
        mAskedHeight = static_cast<std::uint32_t>(std::max(height, 1));

        Rtx::RendererOptions options;
        options.mShaderDirectory = spec.mResourceDir / "rtx" / "shaders";
        options.mCacheDirectory = spec.mCachePath;
        options.mWidth = mAskedWidth;
        options.mHeight = mAskedHeight;
        options.mUpscale = upscale;
        options.mPreset = preset;
        options.mWindow = mWindow;
        // **The run's answer where a run was installed, and the build's otherwise.** A launcher
        // making a measurement says on its command line whether the layers load, because a figure
        // taken under them is not one to compare against anything; a played session has only the
        // build to go on, which `Rtx::sValidationByDefault` says is the one thing that should
        // decide it.
        options.mValidation
            = mSession != nullptr ? mSession->getValidation() : Rtx::ValidationOptions{ Rtx::sValidationByDefault };

        // **The two finer layers, asked for by name and never on by themselves.** The build decides
        // whether the layers load; these decide what they check, and each costs far more than the
        // core checks do — synchronization validation tracks every access of every resource, and
        // the GPU-assisted layer instruments every shader. They are here because the harness could
        // ask for both and the game could ask for neither, and `Rtx::sValidationByDefault` says why
        // two hosts of one renderer must not disagree about the layers. What they answer is the
        // fault a core-clean run still ends in: a device lost with an address and nothing else.
        //
        // **A window under the GPU-assisted layer loses the device on its own**, which is why the
        // two are separate switches: `vkWaitForFences` comes back `VK_ERROR_DEVICE_LOST` on three
        // runs of four, somewhere inside a minute, with nothing wrong in the frame —
        // `RtxTool::chooseValidation` measured it and leaves the layer off over a window for the
        // same reason. So `OPENMW_RTX_SYNC_VALIDATION` is the one to reach for in the game, and
        // `OPENMW_RTX_GPU_VALIDATION` is there for a session willing to tell the two losses apart.
        options.mValidation.mSynchronization
            = options.mValidation.mSynchronization || askedFor("OPENMW_RTX_SYNC_VALIDATION");
        options.mValidation.mGpuAssisted = options.mValidation.mGpuAssisted || askedFor("OPENMW_RTX_GPU_VALIDATION");

        // Either of them is a kind of validation, so either loads the layer that carries it whatever
        // the build said — which is what lets a Release build be asked one question without being
        // rebuilt.
        options.mValidation.mEnabled
            = options.mValidation.mEnabled || options.mValidation.mSynchronization || options.mValidation.mGpuAssisted;

        // **Cleared for a played session and kept for a measured one.** The hit count is a report's
        // figure — it is what tells "the cell rendered" from "the camera faced away from it" — and
        // nothing a player does ever reads it, so an ordinary session is specialized without the
        // atomic rather than writing a number to a buffer nobody looks at, once per pixel that hit
        // anything, for the life of the session.
        options.mCountHits = mSession != nullptr;

        // **The knobs a measurement turns, read where the renderer is built.** They were hard-coded
        // here and taken as command-line options by the harness, so a picture taken by one and a
        // frame drawn by the other were traced by two differently configured renderers.
        options.mCountCrossings = mProfile.mCountCrossings;

        // **Said once, where it is decided.** What reconstructs the frame does not change while the
        // session runs, so it does not belong in the periodic line; what that line carries is the
        // one word a reader of any single line needs, and the rest — which network, at what pair of
        // sizes — is here, where it was chosen.
        Log(Debug::Info) << "Ray tracing: upscale " << Rtx::upscaleName(upscale) << ", Ray Reconstruction preset "
                         << Rtx::presetName(preset);

        // **Grass hangs off the quad tree, and this renderer has the game build none.** Its ground
        // is the cell ring's, and a quad tree beside it would build chunks nothing traces; a setting
        // that wants one is refused by name rather than honoured by a rasterizer's route.
        if (Settings::groundcover().mEnabled)
            throw std::runtime_error("groundcover is on, and the ray tracing renderer builds no quad tree to carry it");

        std::string reason;
        mRenderer = Rtx::createRenderer(options, reason);
        if (mRenderer == nullptr)
            throw std::runtime_error("no ray tracing renderer: " + reason);

        Log(Debug::Info) << "Ray tracing on " << mRenderer->describeDevice();

        // **The renderer's own extent and not SDL's.** A windowed backend sizes itself to the
        // surface, and on a scaled or tiling compositor that is not what the window was asked for.
        // Everything above reads the viewport, so it has to be told what was actually built.
        fitToWindow();

        // **The negative test, and it is the whole claim of this path in one line.** Nothing above
        // here may have made a GL context: not the window, not a realize operation, not an
        // `osgViewer` that slipped back in. A context that exists is one something is paying for.
        if (SDL_GL_GetCurrentContext() != nullptr)
            throw std::runtime_error("something initialised OpenGL under the ray tracing renderer");

        // **The clock everything in the frame is measured by**, and the last thing that would
        // otherwise run on the wall. The eye adapts in real time and the upscaler tunes itself
        // against how fast a motion vector was travelled, so a played session leaves this empty and
        // each reader times what it is about. A measured run cannot: two runs of one build then
        // adapt by different amounts and draw different pictures — measured, 48% of the frame moved
        // by up to 29 of 255 between two runs of one binary.
        if (const float step = Settings::rtx().mFixedStep; step > 0.0f)
            mClock = Rtx::FrameClock(step);

        // **The same step decides whether the ground waits, unless the run says otherwise.** A
        // composite comes back whenever the baker finishes it, so which frame it lands on is a
        // thread's answer rather than the schedule's, and a run whose pictures are compared with
        // another's cannot have that.
        //
        // **The step and not what a run does with its frames.** `shot` and `verify` are what the
        // reference pictures are made with and neither of them hashes a frame, so a condition
        // asking about hashes would leave out the two runs that most need this: measured on
        // `balmora`, four processes drew four different frames after half a second of warming and
        // one frame after a tenth of one.
        //
        // **And a run that means to time the streaming path overrides it**, because waiting is
        // most of what that path then measures. `Rtx::SessionRequest::mSettled` says what the
        // override costs and what it buys.
        const std::optional<bool> stated = mSession != nullptr ? mSession->getSettled() : std::nullopt;
        mMirror.setSettled(stated.value_or(mClock.getStatedStep().has_value()));
    }

    // Out of line because the members it destroys are only forward declared in the header.
    RtxRenderer::~RtxRenderer()
    {
        // Before the renderer, because a write still on the queue holds an image of a frame this
        // owns the memory for.
        mCapture.stop();

        mRenderer.reset();

        if (mWindow != nullptr)
            SDL_DestroyWindow(mWindow);
    }

    void RtxRenderer::createWindow(const std::filesystem::path& resourceDir, const bool hidden)
    {
        // **The backend's own flag, and no `SDL_GL_SetAttribute` anywhere near it.** Which flag a
        // surface needs is the one thing about the API this file would otherwise have had to know,
        // and `Rtx::surfaceWindowFlag` is where that is settled. No GL context is ever made, which
        // is the point of the whole path.
        const WindowPlacement placement = describeWindow(Rtx::surfaceWindowFlag());

        // **Hidden and not absent.** A surface still needs a window, and a swapchain built on one
        // nobody is looking at costs a present per frame and nothing else — so a headless run is
        // the same renderer rather than a second path through it. `SDL_WINDOW_HIDDEN` also keeps
        // the compositor from raising a window over whatever the person running it is doing.
        const Uint32 flags = hidden ? (placement.mFlags | SDL_WINDOW_HIDDEN) : placement.mFlags;

        mWindow = SDL_CreateWindow("OpenMW", placement.mX, placement.mY, placement.mWidth, placement.mHeight, flags);
        if (mWindow == nullptr)
            throw std::runtime_error(std::string("failed to create SDL window: ") + SDL_GetError());

        MWRender::setWindowIcon(*mWindow, resourceDir);
    }

    TerrainPlan RtxRenderer::getTerrainPlan() const
    {
        return TerrainPlan{
            .mPaged = true,
            .mChunks = false,
            .mObjectPaging = false,
            .mCompositeMapLevel = Terrain::sNoCompositeMap,
        };
    }

    void RtxRenderer::enableReference(const ESM::RefNum refnum, const bool enabled)
    {
        mMirror.getRing().setReferenceEnabled(refnum, enabled);
    }

    void RtxRenderer::detachWorld()
    {
        mMirror.detach();
    }

    float RtxRenderer::getTerrainViewDistance(float, float) const
    {
        return landReach();
    }

    void RtxRenderer::attachWorld(RenderingManager& world, osg::Group& worldRoot)
    {
        // Only for the pictures inside the interface: a doll resolves its own textures, and this is
        // where they come from. Nothing about the frame needs it — the mirror is handed an image
        // manager by whoever drives it.
        mResources = world.getResourceSystem();
        mMirror.attach(*mResources);

        // Nothing goes between the world and the screen: what the trace writes is the picture.
        setSceneRoot(worldRoot);
    }

    void RtxRenderer::setSceneRoot(osg::Group& root)
    {
        // Which is also what puts the root under the camera an intersection visitor is accepted on;
        // see `Stage::setSceneRoot`.
        mStage.setSceneRoot(root);
    }

    double RtxRenderer::beginFrame(const double measured)
    {
        mClock.advance(measured);

        return mClock.getStep();
    }

    void RtxRenderer::advance(double simulationTime)
    {
        const double previousReferenceTime = mStage.getFrameStamp().getReferenceTime();
        const unsigned int previousFrame = mStage.getFrameStamp().getFrameNumber();

        mStage.getFrameStamp().setFrameNumber(previousFrame + 1);

        // **What OpenMW ages its caches by**, which is why it comes from the frame's own clock and
        // not from the wall. `Rtx::FrameClock` says what reading the wall here cost.
        mStage.getFrameStamp().setReferenceTime(mClock.getNow());
        mStage.getFrameStamp().setSimulationTime(simulationTime);

        // The same two the viewer writes, because the profiler's own spans are reported against
        // them and a frame with neither reads as a frame that took no time. **A run that states a
        // step reports that cadence here**, because these are read off the stamp and the stamp is
        // what the run stated — what the frames really cost is what `Bench` prints beside them.
        if (mStage.getStats().collectStats("frame_rate"))
        {
            const double spent = mStage.getFrameStamp().getReferenceTime() - previousReferenceTime;
            mStage.getStats().setAttribute(previousFrame, "Frame duration", spent);
            mStage.getStats().setAttribute(previousFrame, "Frame rate", spent > 0.0 ? 1.0 / spent : 0.0);
            mStage.getStats().setAttribute(
                mStage.getFrameStamp().getFrameNumber(), "Reference time", mStage.getFrameStamp().getReferenceTime());
        }
    }

    void RtxRenderer::eventTraversal()
    {
        // **Drained and dropped.** What SDL puts in here is the function keys, which upstream reads
        // with `osgViewer` handlers this renderer does not have; everything the game itself acts on
        // came through `SDLUtil::InputWrapper` and MyGUI long before this. Leaving the queue to grow
        // is the only way to get this wrong.
        osgGA::EventQueue::Events events;
        mStage.getEvents().takeEvents(events);
    }

    void RtxRenderer::tickSchedule()
    {
        if (mSession != nullptr)
            mSession->beforeFrame();
    }

    void RtxRenderer::updateTraversal()
    {
        // **Before the early return, because a main menu has no scene root.** MyGUI's widget
        // animation, its key repeat, its tooltip timers and its screen faders all hang off this one
        // call, and the other backend gets it from an update callback on a node that is always in
        // the graph.
        //
        // **The frame's own step, and not MyGUI's timer.** That timer is a wall clock read in whole
        // milliseconds, and a hit's red overlay faded by it — so two runs of one build drew the
        // overlay at different strengths on the same frame. Measured on `one-cell-walk`: 13 of 360
        // frames differed, and none do now.
        if (MyGUIRtx::RenderManager* gui = MyGUIRtx::RenderManager::getInstancePtr())
            gui->update(static_cast<float>(mClock.getStep()));

        if (!mStage.hasSceneRoot())
            return;

        mUpdateVisitor->reset();
        mUpdateVisitor->setFrameStamp(&mStage.getFrameStamp());
        mUpdateVisitor->setTraversalNumber(mStage.getFrameStamp().getFrameNumber());

        // **Not behind a loading screen.** What the rasterizer says with a blanked traversal mask
        // this says by not walking. The eye below still updates, as it does under that blanked mask:
        // the master camera's own bits are not among the ones it clears.
        if (drawsWorld())
            mStage.getSceneRoot().accept(*mUpdateVisitor);

        // **And the eye, which is not in the graph.** `MWRender::Camera` puts where the player is
        // looking onto the master camera from an update callback, exactly as the viewer's own update
        // traversal reaches it. Without this the view matrix is whatever it was made with, and every
        // frame is traced from the origin looking down.
        //
        // Through the stage, because the stage is what parented the world under that camera and so
        // is what knows why accepting on it would walk the world twice.
        mStage.updateEye(*mUpdateVisitor);
    }

    void RtxRenderer::fitToWindow()
    {
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(mWindow, &width, &height);

        const osg::Timer_t now = osg::Timer::instance()->tick();
        const auto wide = static_cast<std::uint32_t>(std::max(width, 1));
        const auto high = static_cast<std::uint32_t>(std::max(height, 1));

        if (wide != mAskedWidth || high != mAskedHeight)
        {
            mAskedWidth = wide;
            mAskedHeight = high;
            mAskedSince = now;
        }

        if (osg::Timer::instance()->delta_s(mAskedSince, now) < sSettleSeconds)
            return;

        // **Handed over on every settled frame, because whether it changes anything is the
        // backend's to say.** It knows two things this does not: the extent the surface settled on,
        // which is not always the one it was asked for, and whether the swapchain has been told it
        // is stale. Stopping here on the window's own size would answer the first wrongly and would
        // never rebuild for the second — a swapchain that went stale without moving then failed its
        // present for good. `Presenter::wantsResize` says no on a comparison where neither has
        // happened, which is what makes handing it over every frame cost a comparison.
        mRenderer->resize(mAskedWidth, mAskedHeight);

        // Whatever the backend settled on, which is what the trace and the GUI are both sized to.
        const Rtx::FrameExtents extents = mRenderer->getExtents();
        mStage.getCamera().setViewport(
            0, 0, static_cast<int>(extents.mOutputWidth), static_cast<int>(extents.mOutputHeight));
    }

    void RtxRenderer::drawGui()
    {
        // **Between the frame and the present**, because the GUI goes over the finished picture and
        // its colours are display-referred — they were picked looking at a monitor, and a tone curve
        // meant for radiance is how a menu comes out grey.
        if (MyGUIRtx::RenderManager* gui = MyGUIRtx::RenderManager::getInstancePtr())
            gui->collectDrawCalls();
    }

    TracedRun RtxRenderer::describeRun()
    {
        return TracedRun{
            .mBackend = *mRenderer,
            .mViews = *this,
            .mScene = mMirror.getScene(),
            .mWalked = mFound,
            .mWalkedAgain = mFoundAgain,
            .mResources = mResources,
            .mSceneRoot = mStage.hasSceneRoot() ? &mStage.getSceneRoot() : nullptr,
            .mUnreadableTextures = mUnreadable,
        };
    }

    std::optional<PoseMoment> RtxRenderer::describePose()
    {
        if (mResources == nullptr)
            return std::nullopt;

        return PoseMoment{
            .mStamp = mStage.getFrameStamp(), .mFrame = mFrame, .mImages = *mResources->getImageManager()
        };
    }

    void RtxRenderer::deferRedraw(TracedView& view)
    {
        if (std::find(mDeferred.begin(), mDeferred.end(), &view) == mDeferred.end())
            mDeferred.push_back(&view);
    }

    void RtxRenderer::forgetView(TracedView& view)
    {
        std::erase(mDeferred, &view);

        // Nulled rather than erased: a flush may be walking this, and a view that went away from
        // inside one must not move the elements after it.
        std::replace(mDrawing.begin(), mDrawing.end(), &view, static_cast<TracedView*>(nullptr));
    }

    void RtxRenderer::drawDeferredViews()
    {
        if (mDeferred.empty())
            return;

        mDrawing.swap(mDeferred);
        mDeferred.clear();

        for (TracedView* view : mDrawing)
            if (view != nullptr)
                view->redraw();

        mDrawing.clear();
    }

    /// **The frame the trace made, on the screen, before the call that made it returns.** No
    /// composite, no interop and no rasterized frame underneath, which is what takes an interop
    /// path's frame of latency out.
    ///
    /// **Traced or not, the frame is presented**, which is why the world's path ends here as well as
    /// the interface's. A walk that placed nothing, an eye with no roll and a world nobody is being
    /// shown are all reasons to leave the target as it is; none is a reason to stop feeding the
    /// surface, and a window that stops answering is one the compositor eventually says so about.
    /// What the GUI goes over is then the last frame traced, or black where nothing has been — a
    /// main menu, or the moment before the first cell finishes loading.
    void RtxRenderer::presentWithGui()
    {
        const std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();

        drawGui();

        // **A present that failed is a swapchain to rebuild, and `renderFrame` is where that
        // happens.** It asks the window its size before every frame and hands over whatever has
        // settled, so a surface that went stale where the window is standing still is rebuilt on the
        // next frame. One that went stale mid-gesture waits for the gesture, which is the frozen
        // picture a window being dragged shows anyway.
        mRenderer->presentFrame();

        // Summed and not assigned: a loading screen presents through `renderGui` as often as it
        // likes between two traces, and every one of those is inside the frame the next row is for.
        const std::chrono::steady_clock::time_point ended = std::chrono::steady_clock::now();
        mSpan.addPresent(Rtx::since(began, ended));

        mSpan.leave(ended);
    }

    void RtxRenderer::renderGui()
    {
        presentWithGui();
    }

    void RtxRenderer::capture(osg::Image& image, int width, int height)
    {
        mCapture.thumbnail(*mRenderer, image, width, height);
    }

    void RtxRenderer::saveScreenshot()
    {
        mCapture.screenshot(*mRenderer);
    }

    std::unique_ptr<OffscreenView> RtxRenderer::createOffscreenView(const OffscreenViewSpec& spec)
    {
        return std::make_unique<TracedView>(spec, *this, mMirror.getTraversals());
    }

    void RtxRenderer::setVSync(SDLUtil::VSyncMode mode)
    {
        mRenderer->setVerticalSync(mode);
    }

    void RtxRenderer::setUpscale(Rtx::Upscale upscale)
    {
        try
        {
            mRenderer->setUpscale(upscale);
        }
        catch (const Rtx::Error& what)
        {
            // **Reported and then left alone.** What asks is somebody choosing from a menu, and a
            // machine that cannot run the mode they picked is an answer rather than a fault: the
            // renderer is still drawing under the one it had, and `getUpscale` still says which.
            Log(Debug::Warning) << "Ray tracing kept the upscaler it had: " << what.what();
        }
    }

    MyGUI::ITexture& RtxRenderer::freezeFrame()
    {
        const Rtx::TracedFrame frame = mCapture.read(*mRenderer);

        // **Bottom row first, because that is what the one caller takes.** `LoadingScreen` inverts
        // the widget's own V — `_setUVSet(0, 1, 1, 0)` — since the rasterizer's frozen frame is a
        // copy of the framebuffer and OpenGL puts its bottom row at texel row nought. So a texture
        // handed over in the trace's own order is one the loading screen then turns over: the world
        // the player was in, upside down behind the progress bar, for as long as a cell took to
        // load.
        const osg::ref_ptr<osg::Image> taken = Rtx::frameImage(
            frame, static_cast<int>(frame.mWidth), static_cast<int>(frame.mHeight), Rtx::RowOrder::BottomFirst);

        // A full readback, which a load screen is exactly the moment to afford.
        if (taken != nullptr)
            mFrozenFrame.set(*taken);

        if (mFrozenFrame.getTexture() == nullptr)
        {
            // Nothing has been presented yet, which is the very first load. Black is what a fade
            // from nothing looks like, and it is the honest picture of a world that is not there.
            osg::ref_ptr<osg::Image> black = new osg::Image;
            black->allocateImage(1, 1, 1, GL_RGB, GL_UNSIGNED_BYTE);
            std::memset(black->data(), 0, black->getTotalSizeInBytes());
            mFrozenFrame.set(*black);
        }

        return *mFrozenFrame.getTexture();
    }

    std::unique_ptr<MyGUIPlatform::Platform> RtxRenderer::createGuiPlatform(osg::Group& guiRoot,
        Resource::ImageManager& images, Shader::ShaderManager& shaders, const VFS::Manager& vfs, float scalingFactor,
        VFS::Path::NormalizedView resourcePath, const std::filesystem::path& logPath)
    {
        // **MyGUI over the ray tracer, and nothing of OpenSceneGraph in it.** `guiRoot` is where the
        // rasterizer hangs its GUI camera; there is no graph to hang anything off here, and the
        // backend is called by this renderer's own frame instead — `updateTraversal` for the widget
        // animation and `renderFrame` for the triangles.
        auto manager = std::make_unique<MyGUIRtx::RenderManager>(*mRenderer, &images, scalingFactor);

        return std::make_unique<MyGUIPlatform::Platform>(std::move(manager), &vfs, resourcePath, logPath);
    }

    void RtxRenderer::notifyWorldSpaceChanged()
    {
        // **Told rather than worked out.** The mirror grows and recycles its slots and is never
        // cleared, so a cell load leaves it looking exactly as a step across a room does; the
        // renderer has nothing to notice. `Rtx::Renderer::resetHistory` says what that costs.
        mRenderer->resetHistory();
    }

    void RtxRenderer::renderFrame(const SceneFrame& frame)
    {
        const osg::FrameStamp& when = frame.mWhen;

        // **What the game spent since this renderer last let go of the frame** — its update, its
        // cells arriving and whatever it waits on to get them. It is the one stretch of the loop
        // nothing else measures, and it is timed rather than profiled because most of it is a
        // thread asleep.
        const double updateMs = mSpan.sinceLeft(std::chrono::steady_clock::now());

        mFrame = when.getFrameNumber();

        // **Ahead of the trace and not after the present**, so the frame this draws is the one the
        // window's own extent asked for rather than the one behind it.
        fitToWindow();

        // **A frame with the world hidden is the interface and nothing else.** No walk, because the
        // update traversal did not run either; no trace, because the interface covers every pixel of
        // it; and no sweep, because a walk that did not happen has marked nothing and the sweep
        // would take the world.
        //
        // **The emitter clock stops with it**, which is what a clock of its own is for: it counts
        // the seconds this renderer has shown, so a plume resumes where it left off rather than
        // being handed the loading screen in one step.
        if (!drawsWorld())
        {
            presentWithGui();
            return;
        }

        // **Asked of the camera and not of the session**, because what settles it is whether the eye
        // is the player's, and a session is only the thing that usually makes it not.
        mMirror.setShowsPlayer(MWBase::Environment::get().getWorld()->getRenderingManager()->getCamera()->getMode()
            != Camera::Mode::Static);

        // **Where the benchmark's `walk ms` starts**, because that row means the whole mirror. The
        // harness times the same stretch, which is what lets the two rows be read against each
        // other.
        const std::chrono::steady_clock::time_point walked = std::chrono::steady_clock::now();
        mFound = mMirror.mirror(frame, mFrame);
        const double walkMs = Rtx::since(walked, std::chrono::steady_clock::now());

        // **The same graph again, and it should add nothing.** Only a run that asked pays for it,
        // because a second whole-graph walk is the largest cost a frame has.
        if (mSession != nullptr && mSession->wantsSecondWalk())
            mFoundAgain = mMirror.mirror(frame, mFrame);

        traceWorld(frame, mFound, walkMs, updateMs);

        presentWithGui();

        // **After the frame and not before the walk**, and on the frames the trace refused as well:
        // the walk still ran, so its epoch is still the one the next walk has to be measured
        // against. `WorldMirror::settle` says what each half of it is for.
        mMirror.settle();

        // After the sweep, because the sweep is this renderer's and not the game's.
        mSpan.leave(std::chrono::steady_clock::now());
    }

    void RtxRenderer::traceWorld(
        const SceneFrame& frame, const Rtx::ExtractionStats& found, const double walkMs, const double updateMs)
    {
        const osg::FrameStamp& when = frame.mWhen;
        const osg::Camera& camera = frame.mCamera;
        const WorldState& world = frame.mWorld;

        if (mMirror.getScene().getTables().mPlacements.getPlacedCount() == 0)
            return;

        // **Waited for here, ahead of the placement that would otherwise absorb it.** `placeScene`
        // writes the copy of the tables the frame behind is still tracing, so it waits that frame
        // out before it writes — and left to it the stall lands inside `place ms`, which then reads
        // as placement work rather than as a device the CPU is ahead of. One figure, in `wait ms`,
        // which `Rtx::FrameSamples` carries for the harness and for the game alike so that the two
        // reports can be read against each other.
        //
        // **Before the submit below, which is what keeps the CPU a frame ahead of the device**, and
        // `Rtx::Renderer::finishFrame` says why that is the side of it the order decides. What comes
        // back is the frame behind, so the bench row below carries it beside this frame's wall time.
        //
        // **Timed as well as waited for**, because the fence is not the whole of it: the ring then
        // reads the device's counters and its timestamps and destroys what that frame was the last
        // to read, and none of that is in the figure the device reports.
        const std::chrono::steady_clock::time_point finishing = std::chrono::steady_clock::now();
        const std::optional<Rtx::FrameResult> result = mRenderer->finishFrame();
        const double finishMs = Rtx::since(finishing, std::chrono::steady_clock::now());

        // Placed, appended or rebuilt — the decision, and the describing a rebuild needs, are the
        // harness's too and are written once (`Rtx::SceneUploader`).
        const std::chrono::steady_clock::time_point handing = std::chrono::steady_clock::now();
        const Rtx::SceneUpload handed = mMirror.hand(*mRenderer, frame.mImages);
        const double placeMs = Rtx::since(handing, std::chrono::steady_clock::now());

        mHasScene = true;

        if (handed.mKind == Rtx::SceneUpload::Kind::Rebuilt)
            Log(Debug::Info) << "Ray tracing built " << mMirror.getScene().getTables().mMeshes.getRows().size()
                             << " meshes into " << found.mInstances << " instances with " << found.mLights
                             << " lights, " << found.mDeformed << " of them deforming, and skipped "
                             << found.mSkippedUnknown << " it cannot read";

        mUnreadable += handed.mUnreadable;

        if (handed.mUnreadable > 0)
            Log(Debug::Warning) << "Ray tracing could not read " << handed.mUnreadable << " of " << handed.mDescribed
                                << " textures and drew them grey — a live graph holds textures that were never files";

        // **Before the frame and after the scene**, which is the only moment both are true: a
        // picture inside the interface traces against the world this walk has just handed over.
        //
        // Above the eye, because a picture inside the interface brought its own: an eye the trace
        // cannot look along is no reason to leave a map tile blank.
        drawDeferredViews();

        const Rtx::FrameExtents extents = mRenderer->getExtents();

        // **The matrix and not a look-at, which is what lets the player look at their own feet.**
        // `getViewMatrixAsLookAt` hands back a point one unit ahead of the eye, and Morrowind's
        // cells are far enough out that a float ulp there is a hundredth of a unit: differencing two
        // such points names a direction a fifth of a degree wide that lands somewhere else every
        // time the eye moves. And a direction on its own carries no roll, so it has to borrow the
        // world's up — which has no answer at all for an eye looking straight up or down. The game
        // does both, every time somebody looks at the sky or the floor, and every one of those
        // frames was skipped: the picture stopped and the last one stayed on the screen. A view
        // matrix carries its own basis and neither problem survives it.
        //
        // **The frame's field of view and not the setting's.** `WorldState` carries the one the
        // world settled on, which is the override wherever something asked for one — a zoom, a
        // cutscene, a script — and the setting only where nothing did.
        std::optional<Rtx::Shaders::VisibilityConstants> viewpoint;
        try
        {
            viewpoint = Rtx::makeCameraFromView(camera.getViewMatrix(), frame.mEye.mFieldOfView, extents.mRenderWidth,
                extents.mRenderHeight, sNear, Rtx::sFarPlane);
        }
        catch (const Rtx::Error& what)
        {
            // **Asked of the builder rather than tested for here**, which is how the test this
            // replaced came to reject a camera that was perfectly good: it was a copy of a contract
            // that then had two places to be right. Once, because a camera nobody filled in and a
            // real defect look identical from here until it is said how often it happens.
            if (!mComplained)
            {
                mComplained = true;
                Log(Debug::Warning) << "Ray tracing skipped a frame: " << what.what();
            }
            return;
        }

        Rtx::Shaders::VisibilityConstants constants = *viewpoint;

        const Rtx::WorldReading read = readWorld(
            world, mMirror.getSky(), mMirror.getMoonFaces(), landReach(), static_cast<float>(when.getSimulationTime()));

        const float exposureBias = Rtx::describeWorld(read, constants);

        // **What the sampler and the jitter are walked by, and leaving it at zero is a bug with two
        // faces.** The bounce samples the same point every frame, so nothing ever converges; and the
        // upscaler, which jitters whatever it is told, is handed the same sub-pixel offset every
        // frame and reconstructs from one sample taken repeatedly. The harness had exactly this, and
        // it cost a picture that looked plausible and carried none of the detail it was paying for.
        //
        // **The stop's own count where a run is being made, and the game's frame number
        // otherwise.** `Session::getSampleFrame` says why: a measured run has to walk the same
        // sequence twice, and a game's frame number carries the loading screen's frames with it.
        const std::optional<std::uint32_t> sample = mSession != nullptr ? mSession->getSampleFrame() : std::nullopt;
        constants.mFrame = sample.value_or(static_cast<std::uint32_t>(mFrame));

        // **The schedule's and not the profile's**, because a warm-up is not averaged in — a picture
        // of a half-built cell in the sum is what `Session::getAccumulated` exists to keep out.
        const std::uint32_t accumulated = mSession != nullptr ? mSession->getAccumulated() : 0;

        // **The game set neither of these, and the de-lighting is what that cost.**
        // `Rtx::makeCameraFromView` names every field it fills and leaves the rest
        // value-initialised, so a played frame ran at `mDelight` nought — which `texturing.glsl`
        // short-circuits on, handing the trace Bethesda's textures with their painted lighting
        // still in them. The harness set it from `--delight` and defaulted to one, so the two hosts
        // had been lighting the same world by different rules.
        constants.mDelight = mProfile.mDelight;
        constants.mShowAlbedo = mProfile.mShowAlbedo ? 1u : 0u;

        // **Measured, or held where `[RTX] exposure` names a number.** A picture wants the exposure
        // the frame asks for; holding it is what a reference and a pixel test want. Without a
        // measured one an interior lit by nothing but this placeholder's ambient reaches the screen
        // at a few hundredths and reads as black.
        //
        // **The bias is carried rather than worked out here**, because a room is the exception to
        // the rule that would derive it — `Rtx::Skylight::mExposureBias`. Whichever light this cell
        // got settled it, and a second derivation at the frame is a second place to get the
        // exception wrong.
        //
        // **Timed, because a profiler cannot read it.** The record and the submit are almost
        // entirely inside the driver, which carries no frame pointer, so perf attributes what they
        // cost to an address with no caller. `Rtx::Timing::Trace` says what the row is for.
        const std::chrono::steady_clock::time_point tracing = std::chrono::steady_clock::now();

        const Rtx::Reconstruction reconstruction = mRenderer->renderFrame(
            constants, Rtx::FrameOptions::forFrame(mProfile, accumulated, mClock.getStatedStep(), exposureBias));

        // **The whole frame, measured between one trace and the next.** Everything the game does
        // in between is in it — update, cull, this — which is what a player feels and what the
        // wait on the device on its own cannot say.
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        const std::optional<double> since = mSpan.enter(now);
        const double presentMs = mSpan.takePresent();

        if (since.has_value())
        {
            const double frameMs = *since;
            const bool rebuilt = handed.mKind == Rtx::SceneUpload::Kind::Rebuilt;

            if (mSession != nullptr && result.has_value())
            {
                Rtx::FrameSpend spend;
                spend.at(Rtx::Timing::Finish) = finishMs;
                spend.at(Rtx::Timing::Walk) = walkMs;
                spend.at(Rtx::Timing::Fold) = found.mFoldMs;
                spend.at(Rtx::Timing::Place) = placeMs;
                spend.at(Rtx::Timing::Bake) = handed.mBakeMs;
                spend.at(Rtx::Timing::Textures) = handed.mTexturesMs;
                spend.at(Rtx::Timing::Upload) = handed.mUploadMs;
                spend.at(Rtx::Timing::Trace) = Rtx::since(tracing, now);
                spend.at(Rtx::Timing::Present) = presentMs;
                spend.at(Rtx::Timing::Update) = updateMs;

                mSession->frame(describeRun(), *result, frameMs, spend, rebuilt);
            }

            // **Every frame and not the ones the device answered for**, because what this reads is
            // the wall between two traces and the device's answer is not part of it. Once a
            // second, which is how often `Rtx::FrameRate` closes a line — and the window is asked
            // then whether anybody can see it, rather than a copy of that being kept here.
            if (const std::string_view title = mSpeed.addFrame(frameMs);
                !title.empty() && (SDL_GetWindowFlags(mWindow) & SDL_WINDOW_HIDDEN) == 0)
                SDL_SetWindowTitle(mWindow, title.data());
        }

        // **Counted where it is summed**, because `finishFrame` answers nothing until a frame it
        // put in flight comes back. Counting every frame instead divided the total by frames that
        // had contributed nothing to it, so the average read low by a factor nobody could see.
        if (result.has_value() && mSpeed.addWait(result->mWaitMs))
        {
            const Rtx::SceneTables scene = mMirror.getScene().getTables();

            // **The emitters among it, because they are the half a placement count does not carry.**
            // Sprites are not instances and never enter that number, so a cell whose every flame,
            // brazier and raindrop had stopped read exactly like one whose emitters were running.
            Log(Debug::Info) << "Ray tracing: waited " << mSpeed.getWaitMs()
                             << " ms a frame for the device over the last " << mSpeed.getFrames() << ", tracing "
                             << scene.mPlacements.getPlacedCount() << " instances and " << scene.mEmitters.size()
                             << " emitters holding " << scene.mSprites.size() << " sprites at " << extents.mRenderWidth
                             << "x" << extents.mRenderHeight << ", reconstructed by "
                             << Rtx::denoiserName(reconstruction.mDenoiser) << " to " << extents.mOutputWidth << "x"
                             << extents.mOutputHeight;
        }
    }
}
