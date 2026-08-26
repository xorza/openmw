#include "rtxrenderer.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>
#include <limits>
#include <optional>
#include <stdexcept>

#include <SDL.h>

#include <osg/Camera>
#include <osg/FrameStamp>
#include <osg/Image>
#include <osg/Matrixf>
#include <osg/Node>
#include <osg/Stats>
#include <osg/Timer>

#include <osgDB/ReaderWriter>
#include <osgDB/Registry>

#include <osgGA/EventQueue>

#include <osgUtil/UpdateVisitor>

#include <MyGUI_ITexture.h>
#include <MyGUI_RenderManager.h>

#include <components/debug/debuglog.hpp>
#include <components/myguiplatform/myguiplatform.hpp>
#include <components/myguirtx/rendermanager.hpp>
#include <components/nifosg/nifloader.hpp>
#include <components/rtx/camera.hpp>
#include <components/rtx/distantland.hpp>
#include <components/rtx/error.hpp>
#include <components/rtx/fogbuilder.hpp>
#include <components/rtx/frameimage.hpp>
#include <components/rtx/frameworld.hpp>
#include <components/rtx/lightbuilder.hpp>
#include <components/rtx/moonbuilder.hpp>
#include <components/rtx/png.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>
#include <components/rtx/sceneuploader.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/upscale.hpp>
#include <components/sceneutil/screencapture.hpp>
#include <components/sdlutil/imagetosurface.hpp>
#include <components/settings/values.hpp>
#include <components/terrain/chunkmanager.hpp>

#include "../offscreenview.hpp"
#include "../sceneframe.hpp"
#include "../stage.hpp"
#include "../windowsetup.hpp"
#include <components/resource/resourcesystem.hpp>
#include <components/weather/precipitation.hpp>

#include "../renderingmanager.hpp"
#include "../screenshotwriter.hpp"
#include <components/sceneutil/vismask.hpp>

#include "tracedview.hpp"

namespace MWRender
{
    namespace
    {
        /// What every walk this renderer makes takes.
        ///
        /// **An exclusion of what the ray tracer draws itself**, and never a selection of what a
        /// walk is interested in. A node mask is AND-ed at every node on the way down, so the bits
        /// have to be read as "which categories may be seen at all" — which is a different question
        /// from "which subtree am I walking", and that one is answered by where the walk starts.
        ///
        /// Conflating the two is a silent, total failure: naming `SceneUtil::Mask_WeatherParticles` here to
        /// mean "the weather subtree" extracted every storm in the game with all of its particles
        /// missing, because `Resource::SceneManager` marks a `ParticleSystem` drawable
        /// `SceneUtil::Mask_ParticleSystem` and a blizzard's own particles are not categorised as weather.
        ///
        /// The sky, the sun and the simple water are what this renderer draws for itself. What the
        /// content says is not there is a second exclusion, and it is asked of the loader that
        /// stamped it rather than named again here — see where this is installed.
        constexpr osg::Node::NodeMask sWorldTraversal = ~static_cast<osg::Node::NodeMask>(
            SceneUtil::Mask_Sky | SceneUtil::Mask_Sun | SceneUtil::Mask_SimpleWater);
    }

    namespace
    {
        /// Far enough to cross any cell. A primary ray that reaches this has left the world.
        constexpr float sFar = 200000.0f;

        /// A quarter of a Morrowind foot. Nothing is clipped against it — see `mNear` — so it only
        /// has to be nearer than anything the eye can find itself inside of.
        constexpr float sNear = 1.0f;

        /// How often the trace's running average is reported. Five seconds at sixty frames.
        constexpr std::uint32_t sReportEvery = 300;

        /// How many frames `OPENMW_RTX_SHOT` writes before it stops. A cap rather than a count,
        /// because the alternative to a cap is filling a disk with a run somebody forgot about.
        constexpr std::uint32_t sKeepAtMost = 16;

    }

    RtxRenderer::RtxRenderer(const RendererSpec& spec)
        : mStage(spec.mStage)
        , mCamera(new osg::Camera)
        , mFrameStamp(new osg::FrameStamp)
        , mEvents(new osgGA::EventQueue)
        , mUpdateVisitor(new osgUtil::UpdateVisitor)
        , mStats(new osg::Stats("Viewer"))
        , mStartTick(osg::Timer::instance()->tick())
        , mExtractor(std::make_unique<Rtx::SceneExtractor>(mScene, &mTraversals))
    {
        // **What the shader visitor is told a GPU offers.** It runs on every model OpenMW loads
        // and needs a number to fit texture slots into; without a GL context there is nothing to
        // ask. The value only decides how many slots it is willing to use — the roles it labels
        // them with, which is all this renderer reads, are the same either way. The harness has
        // assumed the same number since M0.
        mMaxTextureUnits = 32;

        mFrameStamp->setFrameNumber(0);
        mFrameStamp->setReferenceTime(0.0);
        mFrameStamp->setSimulationTime(0.0);
        mUpdateVisitor->setFrameStamp(mFrameStamp);

        mScreenshotWriter = makeScreenshotWriter(spec.mWorkQueue, spec.mScreenshotPath);

        createWindow(spec.mResourceDir);

        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(mWindow, &width, &height);
        mWidth = static_cast<std::uint32_t>(std::max(width, 1));
        mHeight = static_cast<std::uint32_t>(std::max(height, 1));

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

        Rtx::RendererOptions options;
        options.mShaderDirectory = spec.mResourceDir / "rtx" / "shaders";
        options.mWidth = mWidth;
        options.mHeight = mHeight;
        options.mUpscale = *upscale;
        options.mPreset = *preset;
        options.mWindow = mWindow;
        options.mValidation.mEnabled = Settings::rtx().mValidation;

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

        // **The sky is not mirrored.** It is the one subtree the engine rebuilds every frame —
        // state sets and all — so walking it churns the identity maps, and a sweep that drops four
        // materials a frame bumps the revision and makes every frame a full rebuild. Nothing is
        // lost by leaving it out: a ray that reaches the sky has missed everything, and what it
        // gets then is this renderer's own sky rather than the dome the rasterizer draws.
        //
        // **And `SceneUtil::Mask_SimpleWater` with them, which is a duplicate rather than a subtree to skip.**
        // `MWRender::Water` hangs two coplanar quads under one node — the world's water under
        // `SceneUtil::Mask_Water`, and a deep copy of it under `SceneUtil::Mask_SimpleWater` that exists for the local
        // map — and the rasterizer picks between them with the drawing camera's traversal mask.
        // A mirror that walks both places the sea twice, at the same height, as two meshes.
        //
        // **And what the content hides, which the loader is asked for rather than named twice.**
        // `RenderingManager` installs `SceneUtil::Mask_UpdateVisitor` as the hidden node mask, and a
        // `NifOsg::VisController` animating visibility swaps a node between it and every bit. It is
        // one bit rather than no bits at all so that the update traversal still reaches a hidden
        // bone to animate it — which is why a walk that ignores it traces what nothing draws.
        mExtractor->setTraversalMask(sWorldTraversal & ~NifOsg::Loader::getHiddenNodeMask());

        // What is left of the two is the world's own water, and it is the sea.
        mExtractor->setWaterMask(SceneUtil::Mask_Water);

        // **The negative test, and it is the whole claim of this path in one line.** Nothing above
        // here may have made a GL context: not the window, not a realize operation, not an
        // `osgViewer` that slipped back in. A context that exists is one something is paying for.
        if (SDL_GL_GetCurrentContext() != nullptr)
            throw std::runtime_error("something initialised OpenGL under the ray tracing renderer");

        if (const char* where = std::getenv("OPENMW_RTX_SHOT"); where != nullptr && *where != '\0')
        {
            mKeepAt = where;
            mKeepLeft = sKeepAtMost;
            Log(Debug::Info) << "Ray tracing will write its first " << mKeepLeft << " frames to " << mKeepAt
                             << "-0000.png and on";
        }
    }

    // Out of line because the members it destroys are only forward declared in the header.
    RtxRenderer::~RtxRenderer()
    {
        // Before the renderer, because a write still on the queue holds an image of a frame this
        // owns the memory for.
        if (mScreenshotWriter != nullptr)
            mScreenshotWriter->stop();

        mRenderer.reset();

        if (mWindow != nullptr)
            SDL_DestroyWindow(mWindow);
    }

    void RtxRenderer::createWindow(const std::filesystem::path& resourceDir)
    {
        // **The backend's own flag, and no `SDL_GL_SetAttribute` anywhere near it.** Which one it
        // is — Vulkan here, Metal on Apple silicon — is the one thing about the API this file would
        // otherwise have had to know, and `Rtx::surfaceWindowFlag` is where that is settled. No GL
        // context is ever made, which is the point of the whole path.
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
        return Rtx::distantLandReach();
    }

    void RtxRenderer::attachWorld(RenderingManager& world, osg::Group& worldRoot)
    {
        // Only for the pictures inside the interface: a doll resolves its own textures, and this is
        // where they come from. Nothing about the frame needs it — the mirror is handed an image
        // manager by whoever drives it.
        mResources = world.getResourceSystem();

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

        const std::uint32_t wide = static_cast<std::uint32_t>(std::max(width, 1));
        const std::uint32_t high = static_cast<std::uint32_t>(std::max(height, 1));
        if (wide != mWidth || high != mHeight)
        {
            mWidth = wide;
            mHeight = high;
            mRenderer->resize(mWidth, mHeight);
        }

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

    /// **The frame the trace made, on the screen, before the call that made it returns.** No
    /// composite, no interop and no rasterized frame underneath, which is what takes an interop
    /// path's frame of latency out.
    ///
    /// **Traced or not, the frame is presented.** A walk that placed nothing, an eye with no roll
    /// and a world nobody is being shown are all reasons to leave the target as it is; none is a
    /// reason to stop feeding the surface, and a window that stops answering is one the compositor
    /// eventually says so about.
    void RtxRenderer::finishFrame()
    {
        drawGui();

        if (!mRenderer->presentFrame())
            fitToWindow();
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

    void RtxRenderer::renderGui()
    {
        // **The GUI over whatever the target holds**, which is the last frame traced, or black
        // where nothing has been — a main menu, or the moment before the first cell finishes
        // loading. The surface has to be fed either way or the compositor decides the window has
        // stopped answering.
        drawGui();

        if (!mRenderer->presentFrame())
            fitToWindow();
    }

    Rtx::TracedFrame RtxRenderer::readFrame()
    {
        const Rtx::FrameExtents extents = mRenderer->getExtents();
        if (extents.mOutputWidth == 0 || extents.mOutputHeight == 0)
            return {};

        mRenderer->readPixels(mPixels);

        return Rtx::TracedFrame{
            .mWidth = extents.mOutputWidth,
            .mHeight = extents.mOutputHeight,
            .mPixels = mPixels,
        };
    }

    void RtxRenderer::capture(osg::Image& image, int width, int height)
    {
        // An out-parameter because the caller owns the image, so the shared conversion's result is
        // moved into it rather than handed back.
        const osg::ref_ptr<osg::Image> taken = Rtx::frameImage(readFrame(), width, height, Rtx::RowOrder::BottomFirst);
        if (taken == nullptr)
            return;

        // **Three channels and not four.** The one caller writes a savegame thumbnail as a JPEG,
        // which has no alpha to carry and whose writer refuses a four-channel image outright — an
        // `ERROR_IN_WRITING_FILE` and a save with no picture in it, which is what the rasterizer
        // avoids by reading its own screenshots back as `GL_RGB`.
        image.allocateImage(width, height, 1, GL_RGB, GL_UNSIGNED_BYTE);

        // Row by row, because three bytes a pixel is not a multiple of the packing: a thumbnail 518
        // across is 1,554 bytes of picture in a 1,556-byte row.
        for (int y = 0; y < height; ++y)
        {
            const std::uint8_t* from = taken->data(0, y);
            std::uint8_t* to = image.data(0, y);
            for (int x = 0; x < width; ++x)
                std::memcpy(to + x * 3, from + x * 4, 3);
        }
    }

    void RtxRenderer::saveScreenshot()
    {
        const Rtx::TracedFrame frame = readFrame();

        // Bottom row first, because what writes the file is `osgDB` through the same operation the
        // rasterizer hands `osgViewer`'s captures to, and that is the convention it reads.
        const osg::ref_ptr<osg::Image> taken = Rtx::frameImage(
            frame, static_cast<int>(frame.mWidth), static_cast<int>(frame.mHeight), Rtx::RowOrder::BottomFirst);

        if (taken == nullptr)
        {
            Log(Debug::Warning) << "Ray tracing has no frame to write a screenshot from";
            return;
        }

        // Straight to the writer rather than through a capture handler: the handler's job is to get
        // a frame off the graphics context, and this frame is already off it.
        (*mScreenshotWriter)(*taken, 0);
    }

    std::unique_ptr<OffscreenView> RtxRenderer::createOffscreenView(const OffscreenViewSpec& spec)
    {
        return std::make_unique<TracedView>(spec, *this, *mRenderer);
    }

    MyGUI::ITexture& RtxRenderer::freezeFrame()
    {
        const Rtx::TracedFrame frame = readFrame();

        // The trace's own row order, kept: `MyGUIPlatform::Picture` copies an image straight into a
        // locked texture and the interface draws it from the top down, so flipping here would stand
        // the world the player was in on its head behind the loading screen.
        const osg::ref_ptr<osg::Image> taken = Rtx::frameImage(
            frame, static_cast<int>(frame.mWidth), static_cast<int>(frame.mHeight), Rtx::RowOrder::TopFirst);

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

    void RtxRenderer::keep()
    {
        if (mKeepLeft == 0)
            return;

        --mKeepLeft;

        const Rtx::FrameExtents extents = mRenderer->getExtents();
        mRenderer->readPixels(mPixels);

        const std::filesystem::path file = mKeepAt.string() + std::format("-{:04}.png", sKeepAtMost - mKeepLeft - 1);

        try
        {
            Rtx::writePng(file, extents.mOutputWidth, extents.mOutputHeight, mPixels);
        }
        catch (const std::exception& failed)
        {
            mKeepLeft = 0;
            Log(Debug::Error) << "Ray tracing could not write " << file << ": " << failed.what();
        }
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
            finishFrame();
            return;
        }

        // **The world's clock and not this renderer's.** Everything the graph animates under its own
        // controller reads it off the walk's frame stamp, and the sea off the frame's constants; a
        // clock of our own would run both while the game was paused and neither in step with the
        // time of day.
        mExtractor->setSimulationTime(when.getSimulationTime());

        // **The emitters on their own clock, by the gap and not to the time.** They are the one
        // thing here that integrates the difference between two frames rather than reading the
        // hour, so what a pause or a loading screen leaves in that difference is a jump they would
        // take literally. The extractor clamps it; this only has to hand over the gap.
        mExtractor->advanceEmitters(when.getSimulationTime() - mLastSimulationTime);
        mLastSimulationTime = when.getSimulationTime();

        // **Every frame, and the placements are the one thing it does not throw away.** What goes
        // is the lists a walk refills wholesale — lights, deformed meshes, sprites, emitters. The
        // meshes, materials and texture paths stay because the acceleration structures and the
        // texture array were built from them, and the placements stay because they are addressed by
        // slot: a re-walk over an unchanged graph finds every one of them where it left it.
        mScene.clearPlacement();

        // **The moons' portraits, once, into the table the trace reads.** Held rather than named by
        // a material: a moon is drawn by a ray that reached nothing, so nothing else can speak for
        // the slot and the sweep would take it on the first frame a cell died. The scene outlives
        // every cell here, so this is asked once and never again.
        if (mMoonFaces.mMasser == Rtx::sNoIndex)
        {
            mMoonFaces = Rtx::addMoonFaces(mScene);
            mSkyTextures = Rtx::addSkyTextures(mScene, *mResources->getSceneManager());
        }

        // **What the weather drops, walked as a second root.** Those nodes hang under the sky's
        // camera-relative transform, which strips the translation — so their particles are placed
        // about the origin and the eye is what puts them back. And the sky's own mask keeps the
        // first walk out of that subtree entirely, which is right: a cloud deck is a texture on a
        // ray that reached nothing, and rain is geometry standing in front of one.
        //
        // The same systems the rasterizer draws, not a second set of them. `MWRender::Precipitation`
        // owns them and neither renderer does.
        // Nothing falls where the eye is under water. The rasterizer answers this by not culling
        // the subtree; this renderer answers it by not walking it — off the frame's own answer,
        // which `RenderingManager` already worked out from the water it owns.
        if (frame.mWorld.mPrecipitation != nullptr && !frame.mWorld.mUnderwater)
        {
            osg::Vec3d at;
            osg::Vec3d ahead;
            osg::Vec3d skyward;
            frame.mCamera.getViewMatrixAsLookAt(at, ahead, skyward);

            // **The same mask as everything else, because there is nothing here to select.** The
            // walk starts at the precipitation node, so the subtree is already chosen; the mask is
            // only ever excluding what this renderer draws for itself, and none of that is under
            // here. A set-and-restore around one walk was the shape the mistake came in.
            mExtractor->extract(
                *frame.mWorld.mPrecipitation->getNode(), osg::Matrixf::translate(osg::Vec3f(at)), 0, mFrame);
        }

        // Told once a frame, because what a paged world hides is the frame's to say. Every world
        // walk asks it from here, and the precipitation walk above cannot: it is a subtree.
        mExtractor->follow(frame.mResident);

        const Rtx::ExtractionStats found
            // One walk over the whole graph, where every path is already distinct.
            = mExtractor->extractWorld(frame.mScene, osg::Matrixf::identity(), 0, mFrame);

        const bool traced = traceWorld(frame, found);

        finishFrame();

        // **After the frame and not before the walk.** Where everything stood this frame is what
        // the next one measures its motion against, and saying so any earlier would have this frame
        // comparing itself with itself.
        //
        // On the frames the trace refused as well: the walk still ran, so its epoch is still the
        // one the next walk has to be measured against.
        mExtractor->advance();

        // **What the walk did not find has gone, and this is where the scene is told.** The graph
        // above is the whole world every frame, which is what makes mark and sweep sound here — and
        // it is also the only thing that lets go: the identity maps hold their keys alive, so
        // geometry the engine has dropped outlives it until a sweep takes the entry naming it.
        //
        // Last, because it bumps the epoch the next walk is measured against: everything that
        // survived is still carrying the old stamp until it does.
        if (const Rtx::Retirement went = mExtractor->retire(); !went.empty())
            Log(Debug::Info) << "Ray tracing dropped " << went.mMeshes << " meshes and " << went.mMaterials
                             << " materials the world no longer has";

        // Only what a trace wrote, because the cap is a count of pictures and not of frames: a run
        // that spent its first sixteen at the main menu would write the same black texel sixteen
        // times and have nothing left for the world.
        if (traced)
            keep();
    }

    bool RtxRenderer::traceWorld(const SceneFrame& frame, const Rtx::ExtractionStats& found)
    {
        const osg::FrameStamp& when = frame.mWhen;
        const osg::Camera& camera = frame.mCamera;
        const WorldState& world = frame.mWorld;

        if (mScene.getPlacedCount() == 0)
            return false;

        // Placed, appended or rebuilt — the decision, and the describing a rebuild needs, are the
        // harness's too and are written once (`Rtx::SceneUploader`).
        const Rtx::SceneUpload handed = mUploader.hand(*mRenderer, Rtx::sWorld, mScene, frame.mImages, Rtx::SeaState{});

        mHasScene = true;

        if (handed.mKind == Rtx::SceneUpload::Kind::Rebuilt)
            Log(Debug::Info) << "Ray tracing built " << mScene.getMeshes().size() << " meshes into " << found.mInstances
                             << " instances with " << found.mLights << " lights, " << found.mDeformed
                             << " of them deforming, and skipped " << found.mSkippedUnknown << " it cannot read";

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
            viewpoint = Rtx::makeCameraFromView(
                camera.getViewMatrix(), world.mFieldOfView, extents.mRenderWidth, extents.mRenderHeight, sNear, sFar);
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
        // **Decoded here, because the world does not know what a transport is.** Every colour on
        // the frame is a content file's three bytes over 255 and no transfer function; the
        // rasterizer samples them as they are and this light transport is linear, so the conversion
        // belongs to whichever renderer needs it.
        // **Where the sun *is*, and the light comes back along it.** The world also reports
        // `mSunVector`, which is where the rasterizer's light travels and is not the negation of
        // this — `Sky::sunAt` says why, and why nothing that traces can hold both.
        osg::Vec3f discAt(world.mSunPosition.x(), world.mSunPosition.y(), world.mSunPosition.z());
        if (discAt.length2() > 0.0f)
            discAt.normalize();

        // The horizon is the fog and the zenith is the sky's own, which is the pair Morrowind
        // records: one colour for the air, and one for the dome it fades into overhead.
        const osg::Vec3f haze = Rtx::decodeColour(world.mAir.mColour);

        // **The recorded depth, and not the ramp `MWRender::FogManager` made of it.** That ramp
        // exists to hide a far clip plane and this renderer has no far clip to hide, so per the
        // fork's own rule it does not come across. What is read instead is the number the content
        // wrote, over the distance this picture actually reaches — which is what `openmw-rtxtool`
        // reads out of the same records, so a screenshot and a played frame stand in one air.
        //
        // **A room is measured against a fixed distance and the open air against the world's
        // reach.** The original engine measured both against `viewing distance`, which is shorter
        // than one cell: air tuned to it swallows everything past the active grid, and a world built
        // four cells out then renders identically to one built none. A room takes a constant
        // instead, because how thick the air in a windowless cellar is belongs to the content and
        // not to how much world the player asked for — `Rtx::sInteriorFogReach` says the rest.
        const float reach = world.isInteriorCell() ? Rtx::sInteriorFogReach : Rtx::distantLandReach();

        // **The sun is not assembled here.** Everything the world says about it goes to the one
        // builder that decides what a sun may be — which is what keeps the game and the harness
        // under the same sky, and what makes a sun that lights an empty night impossible to write.
        const Rtx::Skylight sky = Rtx::makeSkylight(Rtx::SkyReading{
            .mSunPosition = discAt,
            .mSunShare = world.mSunDiscColour.a(),
            .mSunColour = Rtx::decodeColour(world.mSunColour),
            .mAmbient = Rtx::decodeColour(world.mAmbientColour),
            .mDiscColour = Rtx::decodeColour(world.mSunDiscColour),
            .mGlare = world.mSunGlare,
        });

        Rtx::FrameWorld described{
            .mSun = sky.mSun,
            .mAmbient = sky.mAmbient,
            .mSkyHorizon = haze,

            // **The sky's own colour, and an interior has none.** The weather system stops writing
            // it the moment the player steps inside, so what the sky is still holding belongs to
            // wherever they were last outdoors — and the air's own colour stands in, which is what a
            // room's sky is anyway. A quasi-exterior is on the outdoor side of that: it has weather.
            .mSkyZenith = world.isOutdoors() ? Rtx::decodeColour(world.mSkyColour) : haze,

            .mAir = { .mColour = haze,
                .mExtinction = Rtx::fogExtinction(world.mFogDepth, reach),

                // **One indoors and nothing out of doors.** Banks are what weather does to a
                // landscape, and a room running the outdoor coverage field reads as a rendering
                // fault rather than as weather.
                .mUniform = world.isInteriorCell() ? 1.0f : 0.0f },

            // Negative infinity and not zero: zero is sea level, and a cell with no water has to
            // answer "how deep is this point" with never.
            .mWaterLevel = world.mWaterEnabled ? world.mWaterHeight : -std::numeric_limits<float>::infinity(),

            // **What the sea is animated by, and leaving it at zero is a frozen ocean.** Real
            // elapsed seconds rather than the frame count: a sea that ran at the frame rate would
            // slow down whenever the frame did.
            .mSeconds = static_cast<float>(when.getSimulationTime()),

            // **The blend runs the opposite way to what its name suggests.** `getWeatherTransition`
            // hands back `WeatherManager::mTransitionFactor`, which is set to one when a change
            // begins and counted *down* as it completes — the engine's own mix is `1 - factor`
            // (`apps/openmw/mwworld/weather.cpp:1261`). Passed through unturned it is a sky that
            // starts as the weather it is becoming and ends as the one it left.
            //
            // **And the current weather twice where nothing is changing**, since the shader mixes
            // unconditionally: naming this one on both sides at a blend of zero is what lets it.
            .mWeather = static_cast<std::uint32_t>(world.mWeatherId),
            .mNextWeather = world.mNextWeatherId.has_value() ? static_cast<std::uint32_t>(*world.mNextWeatherId)
                                                             : static_cast<std::uint32_t>(world.mWeatherId),
            .mWeatherBlend = world.mNextWeatherId.has_value() ? 1.0f - world.mWeatherTransition : 0.0f,

            .mWindSpeed = world.mWindSpeed,

            // Already aimed at the player by the weather system, which is the only thing that knows
            // where they stand. `Rtx::stormDirection` is the same rule for the harness, which has
            // no player to ask.
            .mStormDirection = world.mStormDirection,

            // **The two layers of sky over everything else, and an interior has neither.** Left at
            // their defaults indoors, which is a texture slot of `NO_TEXTURE` and a fade of
            // nothing — the shader skips both before it samples anything.
            .mClouds = world.isOutdoors()
                ? Rtx::describeClouds(static_cast<std::uint32_t>(world.mWeatherId),
                      world.mNextWeatherId.has_value() ? static_cast<std::uint32_t>(*world.mNextWeatherId)
                                                       : static_cast<std::uint32_t>(world.mWeatherId),
                      world.mCloudBlend, world.mAir.mColour, world.mStormDirection, world.mSkyRoll.mClouds,
                      mSkyTextures)
                : Rtx::Shaders::CloudDeck{ .mOpacity = 0.0f,
                      .mTexture = Rtx::Shaders::NO_TEXTURE,
                      .mNext = Rtx::Shaders::NO_TEXTURE },

            .mStars = world.isOutdoors()
                ? Rtx::describeStars(world.mNightFade, world.mSunGlare, world.mSkyRoll.mStars, mSkyTextures)
                : Rtx::Shaders::StarField{ .mTexture = Rtx::Shaders::NO_TEXTURE },
        };

        for (std::size_t moon = 0; moon < described.mMoons.size(); ++moon)
        {
            const MoonState& state = world.mMoons[moon];

            // `Unspecified` is a ninth value and not a phase; the weather system uses it to mean it
            // has not spoken, and a moon it has not spoken about is one with no alpha anyway.
            const int phase = state.mPhase == MoonState::Phase::Unspecified ? 0 : static_cast<int>(state.mPhase);

            described.mMoons[moon] = Rtx::placeMoon(static_cast<Rtx::Moon>(moon), state.mRotationFromHorizon,
                state.mRotationFromNorth, phase, state.mMoonAlpha);
            described.mMoons[moon].mFace = mMoonFaces.of(static_cast<Rtx::Moon>(moon));
        }

        // The nebulae and the constellations, on the star sphere and turning with it. An interior
        // leaves them at their defaults, which is no texture and so nothing drawn.
        if (world.isOutdoors())
            Rtx::describePatches(world.mSkyRoll.mStars, mSkyTextures, described.mSkyPatches);

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
        const Rtx::FrameResult result
            = mRenderer->renderFrame(constants, Rtx::FrameOptions{ .mExposure = std::nullopt });

        // **The whole frame, measured between one trace and the next.** Everything the game does
        // in between is in it — update, cull, the rasterizer, this — which is what a player feels
        // and what `result.mTraceMs` on its own cannot say.
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        if (mEnteredOnce)
            mBench.frame(result, std::chrono::duration<double, std::milli>(now - mEntered).count());

        mEntered = now;
        mEnteredOnce = true;

        mSpentMs += result.mTraceMs;
        if (++mTimed == sReportEvery)
        {
            // **The emitters among it, because they are the half a placement count does not carry.**
            // Sprites are not instances and never enter that number, so a cell whose every flame,
            // brazier and raindrop had stopped read exactly like one whose emitters were running.
            Log(Debug::Info) << "Ray tracing: " << mSpentMs / mTimed << " ms a frame over the last " << mTimed
                             << ", tracing " << mScene.getPlacedCount() << " instances and "
                             << mScene.getEmitters().size() << " emitters holding " << mScene.getSprites().size()
                             << " sprites at " << extents.mRenderWidth << "x" << extents.mRenderHeight
                             << ", reconstructed by " << Rtx::denoiserName(result.mReconstruction.mDenoiser) << " to "
                             << extents.mOutputWidth << "x" << extents.mOutputHeight;
            mSpentMs = 0.0;
            mTimed = 0;
        }

        return true;
    }
}
