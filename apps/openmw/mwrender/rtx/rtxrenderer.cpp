#include "rtxrenderer.hpp"

#include <algorithm>
#include <array>
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
#include <components/rtx/frameimage.hpp>
#include <components/rtx/frametimes.hpp>
#include <components/rtx/frameworld.hpp>
#include <components/rtx/lightbuilder.hpp>
#include <components/rtx/moonbuilder.hpp>
#include <components/rtx/poseupdate.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/sceneuploader.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/upscale.hpp>
#include <components/sceneutil/screencapture.hpp>
#include <components/sceneutil/vismask.hpp>
#include <components/sdlutil/imagetosurface.hpp>
#include <components/settings/values.hpp>
#include <components/surface/material.hpp>
#include <components/terrain/chunkmanager.hpp>

#include "../offscreenview.hpp"
#include "../renderingmanager.hpp"
#include "../sceneframe.hpp"
#include "../screenshotwriter.hpp"
#include "../stage.hpp"
#include "../windowsetup.hpp"
#include "readworld.hpp"
#include "tracedview.hpp"
#include "worldmirror.hpp"

namespace MWRender
{
    namespace
    {
        /// A quarter of a Morrowind foot. Nothing is clipped against it — see `mNear` — so it only
        /// has to be nearer than anything the eye can find itself inside of.
        constexpr float sNear = 1.0f;

        /// How often the trace's running average is reported. Five seconds at sixty frames.
        constexpr std::uint32_t sReportEvery = 300;

        /// How many frames `OPENMW_RTX_SHOT` writes before it stops. A cap rather than a count,
        /// because the alternative to a cap is filling a disk with a run somebody forgot about.
        constexpr std::uint32_t sKeepAtMost = 16;

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
        , mCamera(new osg::Camera)
        , mFrameStamp(new osg::FrameStamp)
        , mEvents(new osgGA::EventQueue)
        , mUpdateVisitor(new Rtx::PoseUpdate)
        , mStats(new osg::Stats("Viewer"))
        , mStartTick(osg::Timer::instance()->tick())
    {
        // **Before any content is read, because it decides what reading one records.** This is the
        // only renderer that asks what the content says a surface is, and the answer is stored on
        // every state set as it is built — so nothing else in the process pays for it.
        Surface::describeSurfaces(true);

        // **One name with the harness, because neither host has a GL context to ask.**
        mMaxTextureUnits = Surface::sAssumedTextureUnits;

        mFrameStamp->setFrameNumber(0);
        mFrameStamp->setReferenceTime(0.0);
        mFrameStamp->setSimulationTime(0.0);
        mUpdateVisitor->setFrameStamp(mFrameStamp);

        createWindow(spec.mResourceDir);

        mStage.adopt(*mCamera, *mFrameStamp, *mEvents, *mStats);

        const std::string wanted = Settings::rtx().mUpscale;

        // **Refused rather than defaulted**, for the reason `Rtx::upscaleNamed` gives: a typo that
        // quietly renders at another mode is a measurement of the wrong thing.
        const std::optional<Rtx::Upscale> upscale = Rtx::upscaleNamed(wanted);
        if (!upscale.has_value())
            throw std::runtime_error('"' + wanted + "\" is not one of off, performance, balanced, quality or dlaa");

        const std::string wantedPreset = Settings::rtx().mPreset;
        const std::optional<Rtx::Preset> preset = Rtx::presetNamed(wantedPreset);
        if (!preset.has_value())
            throw std::runtime_error('"' + wantedPreset + "\" is not one of default, d or e");

        // The window's own size, which `fitToWindow` asks for again on every frame after this one.
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(mWindow, &width, &height);

        Rtx::RendererOptions options;
        options.mShaderDirectory = spec.mResourceDir / "rtx" / "shaders";
        options.mCacheDirectory = spec.mCachePath;
        options.mWidth = static_cast<std::uint32_t>(std::max(width, 1));
        options.mHeight = static_cast<std::uint32_t>(std::max(height, 1));
        options.mUpscale = *upscale;
        options.mPreset = *preset;
        options.mWindow = mWindow;
        options.mValidation.mEnabled = Rtx::sValidationByDefault;

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
        options.mValidation.mSynchronization = askedFor("OPENMW_RTX_SYNC_VALIDATION");
        options.mValidation.mGpuAssisted = askedFor("OPENMW_RTX_GPU_VALIDATION");

        // Either of them is a kind of validation, so either loads the layer that carries it whatever
        // the build said — which is what lets a Release build be asked one question without being
        // rebuilt.
        options.mValidation.mEnabled
            = options.mValidation.mEnabled || options.mValidation.mSynchronization || options.mValidation.mGpuAssisted;

        // **The one place that clears it.** The hit count is a harness figure — `shot` prints it,
        // `bench` reports it, tests assert on it — and nothing in the game ever reads it, so the
        // trace this builds is specialized without the atomic rather than writing a number to a
        // buffer nobody looks at, once per pixel that hit anything, for the life of the session.
        options.mCountHits = false;

        // **Said once, where it is decided.** What reconstructs the frame does not change while the
        // session runs, so it does not belong in the periodic line; what that line carries is the
        // one word a reader of any single line needs, and the rest — which network, at what pair of
        // sizes — is here, where it was chosen.
        Log(Debug::Info) << "Ray tracing: upscale " << Rtx::upscaleName(*upscale) << ", Ray Reconstruction preset "
                         << Rtx::presetName(*preset);

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

        if (const char* where = std::getenv("OPENMW_RTX_SHOT"); where != nullptr && *where != '\0')
            mCapture.keepFrames(where);
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

    void RtxRenderer::createWindow(const std::filesystem::path& resourceDir)
    {
        // **The backend's own flag, and no `SDL_GL_SetAttribute` anywhere near it.** Which flag a
        // surface needs is the one thing about the API this file would otherwise have had to know,
        // and `Rtx::surfaceWindowFlag` is where that is settled. No GL context is ever made, which
        // is the point of the whole path.
        const WindowPlacement placement = describeWindow(Rtx::surfaceWindowFlag());

        mWindow = SDL_CreateWindow(
            "OpenMW", placement.mX, placement.mY, placement.mWidth, placement.mHeight, placement.mFlags);
        if (mWindow == nullptr)
            throw std::runtime_error(std::string("failed to create SDL window: ") + SDL_GetError());

        MWRender::setWindowIcon(*mWindow, resourceDir);
    }

    bool RtxRenderer::wantsPagedTerrain() const
    {
        return true;
    }

    float RtxRenderer::getTerrainCompositeMapLevel() const
    {
        return Terrain::sNoCompositeMap;
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
        mSceneRoot = &root;

        // Which is also what puts the root under the camera an intersection visitor is accepted on;
        // see `Stage::setSceneRoot`.
        mStage.setSceneRoot(root);
    }

    void RtxRenderer::advance(double simulationTime)
    {
        const double previousReferenceTime = mFrameStamp->getReferenceTime();
        const unsigned int previousFrame = mFrameStamp->getFrameNumber();

        mFrameStamp->setFrameNumber(previousFrame + 1);
        mFrameStamp->setReferenceTime(osg::Timer::instance()->delta_s(mStartTick, osg::Timer::instance()->tick()));
        mFrameStamp->setSimulationTime(simulationTime);

        // The same two the viewer writes, because the profiler's own spans are reported against
        // them and a frame with neither reads as a frame that took no time.
        if (mStats->collectStats("frame_rate"))
        {
            const double spent = mFrameStamp->getReferenceTime() - previousReferenceTime;
            mStats->setAttribute(previousFrame, "Frame duration", spent);
            mStats->setAttribute(previousFrame, "Frame rate", spent > 0.0 ? 1.0 / spent : 0.0);
            mStats->setAttribute(mFrameStamp->getFrameNumber(), "Reference time", mFrameStamp->getReferenceTime());
        }
    }

    void RtxRenderer::eventTraversal()
    {
        // **Drained and dropped.** What SDL puts in here is the function keys, which upstream reads
        // with `osgViewer` handlers this renderer does not have; everything the game itself acts on
        // came through `SDLUtil::InputWrapper` and MyGUI long before this. Leaving the queue to grow
        // is the only way to get this wrong.
        osgGA::EventQueue::Events events;
        mEvents->takeEvents(events);
    }

    void RtxRenderer::updateTraversal()
    {
        // **Before the early return, because a main menu has no scene root.** MyGUI's widget
        // animation, its key repeat and its tooltip timers all hang off this one call, and the other
        // backend gets it from an update callback on a node that is always in the graph.
        if (MyGUIRtx::RenderManager* gui = MyGUIRtx::RenderManager::getInstancePtr())
            gui->update();

        if (mSceneRoot == nullptr)
            return;

        mUpdateVisitor->reset();
        mUpdateVisitor->setFrameStamp(mFrameStamp);
        mUpdateVisitor->setTraversalNumber(mFrameStamp->getFrameNumber());

        // **Not behind a loading screen.** What the rasterizer says with a blanked traversal mask
        // this says by not walking. The eye below still updates, as it does under that blanked mask:
        // the master camera's own bits are not among the ones it clears.
        if (drawsWorld())
            mSceneRoot->accept(*mUpdateVisitor);

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

        // **Handed over unguarded, because the guard belongs to the backend.** It knows two things
        // this does not: the extent the surface settled on, which is not always the one it was asked
        // for, and whether the swapchain has been told it is stale. A guard on the window's own size
        // would answer the first wrongly and would never rebuild for the second — a swapchain that
        // went stale at an unchanged size then failed its present for good. `Presenter::resize`
        // returns on a comparison where neither has happened.
        mRenderer->resize(
            static_cast<std::uint32_t>(std::max(width, 1)), static_cast<std::uint32_t>(std::max(height, 1)));

        // Whatever the backend settled on, which is what the trace and the GUI are both sized to.
        const Rtx::FrameExtents extents = mRenderer->getExtents();
        mCamera->setViewport(0, 0, static_cast<int>(extents.mOutputWidth), static_cast<int>(extents.mOutputHeight));
    }

    void RtxRenderer::drawGui()
    {
        // **Between the frame and the present**, because the GUI goes over the finished picture and
        // its colours are display-referred — they were picked looking at a monitor, and a tone curve
        // meant for radiance is how a menu comes out grey.
        if (MyGUIRtx::RenderManager* gui = MyGUIRtx::RenderManager::getInstancePtr())
            gui->collectDrawCalls();
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
    void RtxRenderer::renderGui()
    {
        drawGui();

        // **A present that failed is a swapchain to rebuild, and `renderFrame` is where that
        // happens.** It asks the window its size before every frame and hands it over unguarded, so
        // the rebuild this needs is the one the next frame opens with.
        mRenderer->presentFrame();
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
        return std::make_unique<TracedView>(spec, *this);
    }

    void RtxRenderer::setVSync(SDLUtil::VSyncMode mode)
    {
        mRenderer->setVerticalSync(mode);
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
            renderGui();
            return;
        }

        // **Where the benchmark's `walk ms` starts**, because that row means the whole mirror. The
        // harness times the same stretch, which is what lets the two rows be read against each
        // other.
        const std::chrono::steady_clock::time_point walked = std::chrono::steady_clock::now();
        const Rtx::ExtractionStats found = mMirror.mirror(frame, mFrame);
        const double walkMs = Rtx::since(walked, std::chrono::steady_clock::now());

        const bool traced = traceWorld(frame, found, walkMs);

        renderGui();

        // **After the frame and not before the walk**, and on the frames the trace refused as well:
        // the walk still ran, so its epoch is still the one the next walk has to be measured
        // against. `WorldMirror::settle` says what each half of it is for.
        mMirror.settle();

        // Only what a trace wrote, because the cap is a count of pictures and not of frames: a run
        // that spent its first sixteen at the main menu would write the same black texel sixteen
        // times and have nothing left for the world.
        if (traced)
            mCapture.keep(*mRenderer);
    }

    bool RtxRenderer::traceWorld(const SceneFrame& frame, const Rtx::ExtractionStats& found, double walkMs)
    {
        const osg::FrameStamp& when = frame.mWhen;
        const osg::Camera& camera = frame.mCamera;
        const WorldState& world = frame.mWorld;

        if (mMirror.getScene().getPlacedCount() == 0)
            return false;

        // **Waited for here, ahead of the placement that would otherwise absorb it.** `placeScene`
        // writes the copy of the tables the frame behind is still tracing, so it waits that frame
        // out before it writes — and left to it the stall lands inside `place ms`, which then reads
        // as placement work rather than as a device the CPU is ahead of. One figure, in `wait ms`,
        // and `RtxTool::measurePlace` splits it the same way so the two reports can be read against
        // each other.
        //
        // **Before the submit below, which is what keeps the CPU a frame ahead of the device**, and
        // `Rtx::Renderer::finishFrame` says why that is the side of it the order decides. What comes
        // back is the frame behind, so the bench row below carries it beside this frame's wall time.
        const std::optional<Rtx::FrameResult> result = mRenderer->finishFrame();

        // Placed, appended or rebuilt — the decision, and the describing a rebuild needs, are the
        // harness's too and are written once (`Rtx::SceneUploader`).
        const std::chrono::steady_clock::time_point handing = std::chrono::steady_clock::now();
        const Rtx::SceneUpload handed = mMirror.hand(*mRenderer, frame.mImages);
        const double placeMs = Rtx::since(handing, std::chrono::steady_clock::now());

        mHasScene = true;

        if (handed.mKind == Rtx::SceneUpload::Kind::Rebuilt)
            Log(Debug::Info) << "Ray tracing built " << mMirror.getScene().getMeshes().size() << " meshes into "
                             << found.mInstances << " instances with " << found.mLights << " lights, "
                             << found.mDeformed << " of them deforming, and skipped " << found.mSkippedUnknown
                             << " it cannot read";

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
            viewpoint = Rtx::makeCameraFromView(camera.getViewMatrix(), world.mFieldOfView, extents.mRenderWidth,
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
            return false;
        }

        Rtx::Shaders::VisibilityConstants constants = *viewpoint;

        const WorldRead read = readWorld(
            world, mMirror.getSky(), mMirror.getMoonFaces(), landReach(), static_cast<float>(when.getSimulationTime()));
        const Rtx::FrameWorld described = Rtx::describeWorld(read.mReading);

        Rtx::applyWorld(described, constants);

        // **What the sampler and the jitter are walked by, and leaving it at zero is a bug with two
        // faces.** The bounce samples the same point every frame, so nothing ever converges; and the
        // upscaler, which jitters whatever it is told, is handed the same sub-pixel offset every
        // frame and reconstructs from one sample taken repeatedly. The harness had exactly this, and
        // it cost a picture that looked plausible and carried none of the detail it was paying for.
        constants.mFrame = static_cast<std::uint32_t>(mFrame);

        // **Measured, not held at one.** A picture wants the exposure the frame asks for; holding
        // it is what a reference and a pixel test want, and the default is theirs. Without this an
        // interior lit by nothing but this placeholder's ambient reaches the screen at a few
        // hundredths and reads as black.
        // **The hour is held back only outdoors, because the bias is the hour's and an interior has
        // no hour.** A cell's `AMBI` is dark by the same measure a midnight is, and holding a room
        // back by two stops is not what an eye walking into one does — it adapts to the room.
        // `Rtx::makeRoomLight` is where a room's one is said.
        const float bias
            = read.mExposureBias.value_or(Rtx::exposureBias(described.mSun.mIrradiance, described.mAmbient));

        const Rtx::Reconstruction reconstruction
            = mRenderer->renderFrame(constants, Rtx::FrameOptions{ .mExposureBias = bias, .mExposure = std::nullopt });

        // **The whole frame, measured between one trace and the next.** Everything the game does
        // in between is in it — update, cull, this — which is what a player feels and what the
        // wait on the device on its own cannot say.
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        if (mEnteredOnce && result.has_value())
            mBench.frame(
                *result, Rtx::since(mEntered, now), walkMs, placeMs, handed.mKind == Rtx::SceneUpload::Kind::Rebuilt);

        mEntered = now;
        mEnteredOnce = true;

        if (result.has_value())
            mSpentMs += result->mWaitMs;
        if (++mTimed == sReportEvery)
        {
            // **The emitters among it, because they are the half a placement count does not carry.**
            // Sprites are not instances and never enter that number, so a cell whose every flame,
            // brazier and raindrop had stopped read exactly like one whose emitters were running.
            Log(Debug::Info) << "Ray tracing: waited " << mSpentMs / mTimed
                             << " ms a frame for the device over the last " << mTimed << ", tracing "
                             << mMirror.getScene().getPlacedCount() << " instances and "
                             << mMirror.getScene().getEmitters().size() << " emitters holding "
                             << mMirror.getScene().getSprites().size() << " sprites at " << extents.mRenderWidth << "x"
                             << extents.mRenderHeight << ", reconstructed by "
                             << Rtx::denoiserName(reconstruction.mDenoiser) << " to " << extents.mOutputWidth << "x"
                             << extents.mOutputHeight;
            mSpentMs = 0.0;
            mTimed = 0;
        }

        return true;
    }
}
