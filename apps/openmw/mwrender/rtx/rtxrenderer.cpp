#include "rtxrenderer.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <MyGUI_ITexture.h>
#include <SDL_error.h>
#include <SDL_stdinc.h>
#include <SDL_video.h>
#include <osg/Camera>
#include <osg/FrameStamp>
#include <osg/GL>
#include <osg/Group>
#include <osg/Image>
#include <osg/Matrixf>
#include <osg/Node>
#include <osg/Stats>
#include <osg/Texture2D>
#include <osg/Timer>

#include <components/debug/debuglog.hpp>
#include <components/myguiplatform/myguiplatform.hpp>
#include <components/myguirtx/rendermanager.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/rtx/camera.hpp>
#include <components/rtx/error.hpp>
#include <components/rtx/frameclock.hpp>
#include <components/rtx/frameimage.hpp>
#include <components/rtx/framespend.hpp>
#include <components/rtx/frameworld.hpp>
#include <components/rtx/moonbuilder.hpp>
#include <components/rtx/namedenum.hpp>
#include <components/rtx/poseupdate.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/sceneuploader.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/shaders/visibility.h>
#include <components/rtx/upscale.hpp>
#include <components/rtxvulkan/createrenderer.hpp>
#include <components/sceneutil/screencapture.hpp>
#include <components/sdlutil/imagetosurface.hpp>
#include <components/sdlutil/vsyncmode.hpp>
#include <components/settings/categories.hpp>
#include <components/settings/values.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include "../ground.hpp"
#include "../offscreenview.hpp"
#include "../renderingmanager.hpp"
#include "../rendermode.hpp"
#include "../sceneframe.hpp"
#include "../vismask.hpp"
#include "rtxrun.hpp"
#include "tracedground.hpp"
#include "tracedview.hpp"
#include "worldmirror.hpp"

namespace MWRender
{
    std::uint32_t rayMaskOf(const osg::Node::NodeMask cullMask)
    {
        using namespace SceneUtil;

        std::uint32_t mask = 0;
        if ((cullMask & (Mask_Object | Mask_Static | Mask_Terrain | Mask_Groundcover)) != 0)
            mask |= Rtx::Shaders::MASK_STATIC;
        if ((cullMask & (Mask_Actor | Mask_Player)) != 0)
            mask |= Rtx::Shaders::MASK_ACTOR;
        if ((cullMask & Mask_Effect) != 0)
            mask |= Rtx::Shaders::MASK_EFFECT;
        if ((cullMask & Mask_FirstPerson) != 0)
            mask |= Rtx::Shaders::MASK_FIRST_PERSON;
        if ((cullMask & (Mask_Water | Mask_SimpleWater)) != 0)
            mask |= Rtx::Shaders::MASK_WATER;
        if ((cullMask & (Mask_ParticleSystem | Mask_WeatherParticles)) != 0)
            mask |= Rtx::Shaders::MASK_PARTICLE;

        // No `MASK_MEDIUM`: a medium is gathered by a ray that casts with that bit alone, whatever
        // the camera, and in the eye's own mask it would meet the shells of a class left out.
        return mask;
    }

    namespace
    {
        /// What a played binary runs: the two choices `[RTX]` leaves a player, and for the rest the
        /// one answer a played frame has. The knobs a measurement turns — delight, albedo, the
        /// filter, the exposure, the crossings — are a run's, handed over in `RendererSpec::mRtx`
        /// by the harness that makes one, and a settings file cannot reach them: one that could
        /// once turned a played game into a fixed-step run for good.
        ///
        /// **Here and not in `components/rtx`**, because the settings registry is a global the core
        /// has no other reason to read.
        Rtx::RenderProfile profileFromSettings()
        {
            Rtx::RenderProfile profile;

            profile.mUpscaling.mMode = Rtx::sUpscaleNames.require(Settings::rtx().mUpscale.get(), "an upscale mode");
            profile.mUpscaling.mPreset
                = Rtx::sPresetNames.require(Settings::rtx().mPreset.get(), "a Ray Reconstruction preset");

            // A played session shows every frame and sums none, and measures its exposure off each.
            profile.mRadianceWidth = Rtx::RadianceWidth::Shown;
            profile.mExposure = std::nullopt;

            return profile;
        }

        /// The run a played session is: every answer the played one, and nothing noted from any
        /// frame. One for the process, because a played session has no state a run would keep.
        class PlayedRun final : public RtxRun
        {
        public:
            bool isHeadless() const override { return false; }

            /// The build's, which `Rtx::sValidationByDefault` says is the one thing that should
            /// decide it for a session with no command line.
            const Rtx::ValidationOptions& getValidation() const override { return mValidation; }

            bool wantsHitCounts() const override { return false; }

            /// The wall: the eye adapts in real time and the upscaler tunes itself against how fast
            /// a motion vector was travelled, so each reader times what it is about. A setting that
            /// could state a step once made a played game step by frames, and at two hundred of
            /// them a second the world ran three times over.
            std::optional<float> getStep() const override { return std::nullopt; }

            std::optional<bool> getSettled() const override { return std::nullopt; }
            std::optional<std::uint32_t> getSampleFrame() const override { return std::nullopt; }
            std::uint32_t getAccumulated() const override { return 0; }
            bool wantsSecondWalk() const override { return false; }
            void beforeFrame() override {}
            void frame(const FrameContext& context, const FrameReport& report) override {}

        private:
            Rtx::ValidationOptions mValidation{ Rtx::sValidationByDefault };
        };

        PlayedRun sPlayedRun;

        /// A quarter of a Morrowind foot. Nothing is clipped against it — see `mNear` — so it only
        /// has to be nearer than anything the eye can find itself inside of.
        constexpr float sNear = 1.0f;

        /// How long the window must report one size before the renderer is rebuilt for it.
        ///
        /// **Because rebuilding costs about as long as this waits.** A new extent releases every
        /// target, allocates them again and uploads Ray Reconstruction's weights for the pair of
        /// resolutions it is now between — about a tenth of a second. A window dragged across a
        /// screen passes through hundreds of extents, and following each of them would draw the
        /// drag at ten frames a second.
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

    std::optional<double> RtxRenderer::FrameSpan::enter(const std::chrono::steady_clock::time_point now)
    {
        const std::optional<double> since
            = mEntered.has_value() ? std::optional(Rtx::since(*mEntered, now)) : std::nullopt;
        mEntered = now;
        return since;
    }

    double RtxRenderer::FrameSpan::sinceLeft(const std::chrono::steady_clock::time_point now) const
    {
        return Rtx::since(mLeft, now);
    }

    double RtxRenderer::FrameSpan::takePresent()
    {
        return std::exchange(mPresentMs, 0.0);
    }

    std::string_view RtxRenderer::SpeedReport::addFrame(const double frameMs)
    {
        if (!mRate.add(frameMs))
            return {};

        const auto written = std::format_to_n(mTitle.data(), mTitle.size() - 1, "OpenMW - {}", mRate.getText());
        *written.out = '\0';

        return std::string_view(mTitle.data(), static_cast<std::size_t>(written.out - mTitle.data()));
    }

    bool RtxRenderer::SpeedReport::addWait(const double waitMs)
    {
        mSpentMs += waitMs;
        ++mTimed;

        if (mTimed < sReportEvery)
            return false;

        mReportedMs = std::exchange(mSpentMs, 0.0);
        mReported = std::exchange(mTimed, 0);
        return true;
    }

    RtxRenderer::RtxRenderer(const RendererSpec& spec)
        : mUpdateVisitor(new Rtx::PoseUpdate)
        , mStartTick(osg::Timer::instance()->tick())
        , mRun(spec.mRtx != nullptr ? spec.mRtx->mRun : sPlayedRun)
    {
        // **Made here, because there is no viewer to make them.** Every renderer needs the four and
        // one built on `osgViewer` gets them already wired together.
        const osg::ref_ptr<osg::Camera> camera = new osg::Camera;
        const osg::ref_ptr<osg::FrameStamp> frameStamp = new osg::FrameStamp;
        const osg::ref_ptr<osg::Stats> stats = new osg::Stats("Viewer");

        frameStamp->setFrameNumber(0);
        frameStamp->setReferenceTime(0.0);
        frameStamp->setSimulationTime(0.0);
        mUpdateVisitor->setFrameStamp(frameStamp);

        adopt(*camera, *frameStamp, *stats);

        // **Read before anything is built, because it decides how the window opens and what the
        // trace counts.** A harness hands a whole profile over in the spec; a played binary has
        // none, and runs at what its settings say.
        mProfile = spec.mRtx != nullptr ? spec.mRtx->mProfile : profileFromSettings();

        createWindow(mRun.isHeadless());

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
        options.mWindow = mWindow;
        options.mVerticalSync = Settings::video().mVsyncMode;
        // **The run's answer.** A launcher making a measurement says on its command line whether
        // the layers load, because a figure taken under them is not one to compare against
        // anything; `PlayedRun` says what a session with no command line answers.
        options.mValidation = mRun.getValidation();

        // **The two finer layers, asked for by name and never on by themselves.** The build decides
        // whether the layers load; these decide what they check, and each costs far more than the
        // core checks do — synchronization validation tracks every access of every resource, and
        // the GPU-assisted layer instruments every shader. They are here because the harness could
        // ask for both and the game could ask for neither, and `Rtx::sValidationByDefault` says why
        // two hosts of one renderer must not disagree about the layers. What they answer is the
        // fault a core-clean run still ends in: a device lost with an address and nothing else.
        //
        // **The GPU-assisted layer takes the process down on its own**, which is why the two are
        // separate switches: over a window `vkWaitForFences` comes back `VK_ERROR_DEVICE_LOST` on
        // three runs of four, somewhere inside a minute, with nothing wrong in the frame, and
        // headless it has aborted inside the layer's own thread. `RtxTool::chooseValidation` gives
        // it no default at all for the same reason. So `OPENMW_RTX_SYNC_VALIDATION` is the one to
        // reach for in the game, and `OPENMW_RTX_GPU_VALIDATION` is there for a session willing to
        // tell the losses apart.
        options.mValidation.mSynchronization
            = options.mValidation.mSynchronization || askedFor("OPENMW_RTX_SYNC_VALIDATION");
        options.mValidation.mGpuAssisted = options.mValidation.mGpuAssisted || askedFor("OPENMW_RTX_GPU_VALIDATION");

        // Either of them is a kind of validation, so either loads the layer that carries it whatever
        // the build said — which is what lets a Release build be asked one question without being
        // rebuilt.
        options.mValidation.mEnabled
            = options.mValidation.mEnabled || options.mValidation.mSynchronization || options.mValidation.mGpuAssisted;

        options.mCountHits = mRun.wantsHitCounts();

        // **The knobs a measurement turns, handed over whole where the renderer is built**, so a
        // picture taken by the harness and a frame drawn by the game come from one configuration.
        options.mProfile = mProfile;

        // **Said once, where it is decided.** What reconstructs the frame does not change while the
        // session runs, so it does not belong in the periodic line; what that line carries is the
        // one word a reader of any single line needs, and the rest — which network, at what pair of
        // sizes — is here, where it was chosen.
        Log(Debug::Info) << "Ray tracing: upscale " << Rtx::sUpscaleNames.name(mProfile.mUpscaling.mMode)
                         << ", Ray Reconstruction preset " << Rtx::sPresetNames.name(mProfile.mUpscaling.mPreset);

        // **Grass hangs off the quad tree, and this renderer has the game build none.** Its ground
        // is the cell ring's, and a quad tree beside it would build chunks nothing traces; a setting
        // that wants one is refused by name rather than honoured by a rasterizer's route.
        if (Settings::groundcover().mEnabled)
            throw std::runtime_error("groundcover is on, and the ray tracing renderer builds no quad tree to carry it");

        mRenderer = Rtx::createVulkanRenderer(options);

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
        // otherwise run on the wall. A measured run cannot run on the wall: two runs of one build
        // would adapt by different amounts and draw different pictures. So the step is the run's
        // and nothing else's; `PlayedRun::getStep` says why a played session and a run somebody
        // watches both state none.
        mClock = Rtx::FrameClock(mRun.getStep());

        // **The same step decides whether the ground waits, unless the run says otherwise.** A
        // composite comes back whenever the baker finishes it, so which frame it lands on is a
        // thread's answer rather than the schedule's, and a run whose pictures are compared with
        // another's cannot have that.
        //
        // **The step and not what a run does with its frames.** `shot` is what the reference
        // pictures are made with and it hashes no frame, so a condition asking about hashes would
        // leave out the run that most needs this: measured on
        // `balmora`, four processes drew four different frames after half a second of warming and
        // one frame after a tenth of one.
        //
        // **And a run that means to time the streaming path overrides it**, because waiting is
        // most of what that path then measures. `Rtx::SessionRequest::mSettled` says what the
        // override costs and what it buys.
        mMirror.setSettled(mRun.getSettled().value_or(mClock.getStatedStep().has_value()));
    }

    // Out of line because the members it destroys are only forward declared in the header.
    RtxRenderer::~RtxRenderer()
    {
        // `Engine` has stopped the screenshot writer by now, so no write on the queue still holds
        // an image of a frame this owns the memory for.

        // Its slot is in the renderer's table, so it goes back before the table does.
        mFrozenFrameTexture.reset();

        mRenderer.reset();

        if (mWindow != nullptr)
            SDL_DestroyWindow(mWindow);
    }

    void RtxRenderer::createWindow(const bool hidden)
    {
        // **The backend's own flag, and no `SDL_GL_SetAttribute` anywhere near it.** No GL context is
        // ever made, which is the point of the whole path.
        const WindowPlacement placement = describeWindow(SDL_WINDOW_VULKAN);

        // **Hidden and not absent.** A surface still needs a window, and a swapchain built on one
        // nobody is looking at costs a present per frame and nothing else — so a headless run is
        // the same renderer rather than a second path through it. `SDL_WINDOW_HIDDEN` also keeps
        // the compositor from raising a window over whatever the person running it is doing.
        const Uint32 flags = hidden ? (placement.mFlags | SDL_WINDOW_HIDDEN) : placement.mFlags;

        mWindow = SDL_CreateWindow("OpenMW", placement.mX, placement.mY, placement.mWidth, placement.mHeight, flags);
        if (mWindow == nullptr)
            throw std::runtime_error(std::string("failed to create SDL window: ") + SDL_GetError());
    }

    void RtxRenderer::updateEye(osg::Camera& camera, osgUtil::UpdateVisitor& visitor)
    {
        if (camera.getUpdateCallback() == nullptr)
            return;

        const osg::NodeVisitor::TraversalMode was = visitor.getTraversalMode();
        visitor.setTraversalMode(osg::NodeVisitor::TRAVERSE_NONE);
        camera.accept(visitor);
        visitor.setTraversalMode(was);
    }

    void RtxRenderer::enableReference(const ESM::RefNum refnum, const bool enabled)
    {
        mMirror.setReferenceEnabled(refnum, enabled);
    }

    void RtxRenderer::detachWorld()
    {
        mMirror.detach();
    }

    float RtxRenderer::getGroundReach() const
    {
        return mMirror.getReach();
    }

    void RtxRenderer::prepareResources(Resource::ResourceSystem& resources)
    {
        mResources = &resources;
        resources.getSceneManager()->setShadersEnabled(false);
    }

    osg::ref_ptr<osg::Group> RtxRenderer::createSceneRoot()
    {
        return new osg::Group;
    }

    Ground RtxRenderer::createGround(const GroundSpec& spec)
    {
        Ground ground;
        ground.mTerrain
            = std::make_unique<TracedGround>(spec.mSceneRoot, spec.mStorage, Mask_Terrain, spec.mWorldspace);
        return ground;
    }

    void RtxRenderer::addCell(const MWWorld::CellStore* cell)
    {
        mMirror.standSea(*cell);
    }

    void RtxRenderer::listAssetsToPreload(
        std::vector<VFS::Path::Normalized>& models, std::vector<VFS::Path::Normalized>& textures)
    {
        // What `WorldMirror::attach` reads for its sky: the cloud shell, the star sphere, the two
        // moons' full faces. A missing model aborts the whole preload, and the second star sphere
        // is an expansion's.
        models.push_back(Settings::models().mSkyclouds);
        if (mResources->getVFS()->exists(Settings::models().mSkynight02.get()))
            models.push_back(Settings::models().mSkynight02);
        models.push_back(Settings::models().mSkynight01);

        textures.emplace_back("textures/tx_masser_full.dds");
        textures.emplace_back("textures/tx_secunda_full.dds");
    }

    bool RtxRenderer::toggleRenderMode(const RenderMode mode)
    {
        if (mode == Render_Scene)
            return mWorldToggled = !mWorldToggled;

        return false;
    }

    void RtxRenderer::attachWorld(RenderingManager& world, osg::Group& worldRoot)
    {
        // Straight under the root: the rasterizer hangs its shadowed scene between the two, and
        // this renderer has nothing to put there.
        worldRoot.addChild(world.getSceneRoot());

        // Only for the pictures inside the interface: a doll resolves its own textures. Nothing
        // about the frame needs it — the mirror is handed an image manager by whoever drives it.
        mMirror.attach(*mResources);
    }

    void RtxRenderer::adoptTraversalRoot(osg::Group& root)
    {
        // Under the camera, whose matrices are what put a viewport ray in the world; parented once
        // however often it is said.
        osg::Camera& camera = getCamera();
        if (!camera.containsNode(&root))
            camera.addChild(&root);
    }

    double RtxRenderer::beginFrame(const double measured)
    {
        mClock.advance(measured);

        return mClock.getStep();
    }

    void RtxRenderer::advance(double simulationTime)
    {
        getFrameStamp().setFrameNumber(getFrameStamp().getFrameNumber() + 1);

        // **What OpenMW ages its caches by**, which is why it comes from the frame's own clock and
        // not from the wall. `Rtx::FrameClock` says what reading the wall here cost.
        getFrameStamp().setReferenceTime(mClock.getNow());
        getFrameStamp().setSimulationTime(simulationTime);
    }

    void RtxRenderer::eventTraversal()
    {
        // Nothing to traverse: this renderer adopted no queue, and everything the game acts on came
        // through `SDLUtil::InputWrapper` and MyGUI before this.
    }

    void RtxRenderer::tickSchedule()
    {
        mRun.beforeFrame();
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
        // overlay at different strengths on the same frame.
        assert(mGui != nullptr && "a frame before the interface was made");
        mGui->update(static_cast<float>(mClock.getStep()));

        mUpdateVisitor->reset();
        mUpdateVisitor->setFrameStamp(&getFrameStamp());
        mUpdateVisitor->setTraversalNumber(getFrameStamp().getFrameNumber());

        // **Not behind a loading screen.** What the rasterizer says with a blanked traversal mask
        // this says by not walking. The eye below still updates, as it does under that blanked mask:
        // the master camera's own bits are not among the ones it clears. `tws` is not asked here:
        // under the rasterizer it masks the cull alone and the world keeps animating behind it.
        if (mWorldShown)
            getTraversalRoot().accept(*mUpdateVisitor);

        // **And the eye, which is not in the graph.** `MWRender::Camera` puts where the player is
        // looking onto the master camera from an update callback, exactly as the viewer's own update
        // traversal reaches it. Without this the view matrix is whatever it was made with, and every
        // frame is traced from the origin looking down.
        updateEye(getCamera(), *mUpdateVisitor);
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
        getCamera().setViewport(0, 0, static_cast<int>(extents.mOutputWidth), static_cast<int>(extents.mOutputHeight));
    }

    void RtxRenderer::drawGui()
    {
        // **Between the frame and the present**, because the GUI goes over the finished picture and
        // its colours are display-referred — they were picked looking at a monitor, and a tone curve
        // meant for radiance is how a menu comes out grey.
        assert(mGui != nullptr && "a GUI drawn before the interface was made");
        mGui->collectDrawCalls();
    }

    FrameContext RtxRenderer::describeContext()
    {
        return FrameContext{
            .mRenderer = *this,
            .mResources = mResources,
            .mScene = mMirror.getScene(),
            .mReach = mMirror.getReach(),
            .mEye = mMirror.getEye(),
        };
    }

    std::optional<PoseMoment> RtxRenderer::describePose()
    {
        if (mResources == nullptr)
            return std::nullopt;

        return PoseMoment{ .mStamp = getFrameStamp(), .mFrame = mFrame, .mImages = *mResources->getImageManager() };
    }

    void RtxRenderer::redraw(TracedView& view)
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

    double RtxRenderer::drawViews()
    {
        // **Asked for before there is a world, every time a game starts.** A cell asks for its map
        // tile as it loads, which is the frame before the one that first mirrors it; the tile is
        // drawn when there is something to draw it against rather than left blank until the local
        // map happens to ask again.
        if (mDeferred.empty() || !mHasScene)
            return 0.0;

        const std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();

        mDrawing.swap(mDeferred);
        mDeferred.clear();

        std::uint32_t world = 0;
        for (TracedView* view : mDrawing)
        {
            if (view == nullptr)
                continue;

            // Held over in the order asked, behind nothing asked since: `mDeferred` is empty until
            // the first one is put back.
            if (view->isOfWorld() && world == sWorldViewsPerFrame)
            {
                mDeferred.push_back(view);
                continue;
            }

            if (view->isOfWorld())
                ++world;

            view->draw();
        }

        mDrawing.clear();

        return Rtx::since(began, std::chrono::steady_clock::now());
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

    osg::ref_ptr<osg::Image> RtxRenderer::readFrame(const int width, const int height, const Rtx::Channels channels)
    {
        const Rtx::FrameExtents extents = mRenderer->getExtents();
        if (extents.mOutputWidth == 0 || extents.mOutputHeight == 0)
            return nullptr;

        mRenderer->readPixels(mReadBack);

        const Rtx::TracedFrame frame{
            .mWidth = extents.mOutputWidth,
            .mHeight = extents.mOutputHeight,
            .mPixels = mReadBack,
        };

        return Rtx::frameImage(frame, width > 0 ? width : static_cast<int>(frame.mWidth),
            height > 0 ? height : static_cast<int>(frame.mHeight), Rtx::RowOrder::BottomFirst, channels);
    }

    void RtxRenderer::capture(osg::Image& image, int width, int height)
    {
        const osg::ref_ptr<osg::Image> taken = readFrame(width, height, Rtx::Channels::Rgb);
        if (taken == nullptr)
            return;

        image.swap(*taken);
    }

    void RtxRenderer::saveScreenshot()
    {
        const osg::ref_ptr<osg::Image> taken = readFrame();
        if (taken == nullptr)
        {
            Log(Debug::Warning) << "Ray tracing has no frame to write a screenshot from";
            return;
        }

        getScreenshotWriter()(*taken, 0);
    }

    std::unique_ptr<OffscreenView> RtxRenderer::createWorldView(const OffscreenViewSpec& spec)
    {
        return std::make_unique<TracedView>(spec, nullptr, *this, mMirror.getTraversals());
    }

    std::unique_ptr<SubjectView> RtxRenderer::createSubjectView(const OffscreenViewSpec& spec)
    {
        return std::make_unique<TracedView>(spec, &spec.mScene, *this, mMirror.getTraversals());
    }

    void RtxRenderer::setVSync(SDLUtil::VSyncMode mode)
    {
        mRenderer->setVerticalSync(mode);
    }

    void RtxRenderer::processChangedSettings(const Settings::CategorySettingVector& changed)
    {
        if (changed.contains({ "RTX", "upscale" }))
            setUpscale(Settings::rtx().mUpscale.get());
    }

    /// A name a renderer cannot read, or a mode this machine cannot reach, is reported and left
    /// where it was, because what asks is somebody choosing from a menu.
    void RtxRenderer::setUpscale(const std::string_view name)
    {
        const std::optional<Rtx::Upscale> upscale = Rtx::sUpscaleNames.named(name);
        if (!upscale.has_value())
        {
            Log(Debug::Warning) << "Ray tracing kept the upscaler it had: no mode is named \"" << name << '"';
            return;
        }

        try
        {
            mRenderer->setUpscale(*upscale);
        }
        catch (const Rtx::Error& what)
        {
            // What asks is somebody choosing from a menu, and a machine that cannot run the mode they
            // picked is an answer rather than a fault: the renderer keeps drawing under the one it had.
            Log(Debug::Warning) << "Ray tracing kept the upscaler it had: " << what.what();
        }
    }

    MyGUI::ITexture& RtxRenderer::freezeFrame()
    {
        const osg::ref_ptr<osg::Image> taken = readFrame();

        if (mFrozenFrame == nullptr)
            mFrozenFrame = new osg::Texture2D;

        // A full readback, which a load screen is exactly the moment to afford.
        if (taken != nullptr)
            mFrozenFrame->setImage(taken);

        if (mFrozenFrame->getImage() == nullptr)
        {
            // Nothing has been presented yet, which is the very first load. Black is what a fade
            // from nothing looks like, and it is the honest picture of a world that is not there.
            osg::ref_ptr<osg::Image> black = new osg::Image;
            black->allocateImage(1, 1, 1, GL_RGB, GL_UNSIGNED_BYTE);
            std::memset(black->data(), 0, black->getTotalSizeInBytes());
            mFrozenFrame->setImage(black);
        }

        if (mFrozenFrameTexture == nullptr)
            mFrozenFrameTexture = mGui->shareTexture(*mFrozenFrame);

        return *mFrozenFrameTexture;
    }

    std::unique_ptr<MyGUIPlatform::Platform> RtxRenderer::createGuiPlatform(
        float scalingFactor, VFS::Path::NormalizedView resourcePath, const std::filesystem::path& logPath)
    {
        // **MyGUI over the ray tracer, and nothing of OpenSceneGraph in it.** Nothing is hung in the
        // graph; the backend is called by this renderer's own frame instead — `updateTraversal` for
        // the widget animation and `renderFrame` for the triangles.
        auto manager
            = std::make_unique<MyGUIRtx::RenderManager>(*mRenderer, mResources->getImageManager(), scalingFactor);
        mGui = manager.get();

        return std::make_unique<MyGUIPlatform::Platform>(
            std::move(manager), mResources->getVFS(), resourcePath, logPath);
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

        FrameReport report;

        // **What the game spent since this renderer last let go of the frame** — its update, its
        // cells arriving and whatever it waits on to get them. It is the one stretch of the loop
        // nothing else measures, and it is timed rather than profiled because most of it is a
        // thread asleep.
        report.mSpend.at(Rtx::Timing::Update) = mSpan.sinceLeft(std::chrono::steady_clock::now());

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

        // **Off the frame and not off the session**, because what settles it is whether the eye
        // is the player's, and a session is only the thing that usually makes it not.
        mMirror.setShowsPlayer(frame.mEye.mPlayersEye);

        // **Where the benchmark's `walk ms` starts**, because that row means the whole mirror. The
        // harness times the same stretch, which is what lets the two rows be read against each
        // other.
        const std::chrono::steady_clock::time_point walked = std::chrono::steady_clock::now();
        mWalked.mFound = mMirror.mirror(frame, mFrame);
        report.mSpend.at(Rtx::Timing::Walk) = Rtx::since(walked, std::chrono::steady_clock::now());
        report.mSpend.at(Rtx::Timing::Fold) = mWalked.mFound.mFoldMs;

        // **The same graph again, and it should add nothing.** Only a run that asked pays for it,
        // because a second whole-graph walk is the largest cost a frame has.
        mWalked.mAgain.reset();
        if (mRun.wantsSecondWalk())
            mWalked.mAgain = mMirror.mirror(frame, mFrame);

        traceWorld(frame, report);

        renderGui();

        // **After the frame and not before the walk**, and on the frames the trace refused as well:
        // the walk still ran, so its epoch is still the one the next walk has to be measured
        // against. `WorldMirror::settle` says what each half of it is for.
        mMirror.settle();

        // After the sweep, because the sweep is this renderer's and not the game's.
        mSpan.leave(std::chrono::steady_clock::now());
    }

    void RtxRenderer::traceWorld(const SceneFrame& frame, FrameReport& report)
    {
        if (mMirror.getScene().placements().getPlacedCount() == 0)
            return;

        finishBehind(report);
        handOver(frame, report);

        // **Before the frame and after the scene**, which is the only moment both are true: a
        // picture inside the interface traces against the copy of the tables this walk has just
        // handed over, and the frame's own trace is what stamps that copy as read.
        //
        // Above the eye, because a picture inside the interface brought its own: an eye the trace
        // cannot look along is no reason to leave a map tile blank.
        report.mSpend.at(Rtx::Timing::Views) = drawViews();

        const std::optional<Rtx::Shaders::VisibilityConstants> constants = aim(frame);
        if (!constants.has_value())
            return;

        trace(frame, *constants, report);
    }

    void RtxRenderer::finishBehind(FrameReport& report)
    {
        // **Waited for here, ahead of the placement that would otherwise absorb it.** `placeScene`
        // writes the copy of the tables the frame before last traced, so it waits that frame out
        // before it writes — and left to it the stall lands inside `place ms`, which then reads
        // as placement work rather than as a device the CPU is ahead of. One figure, in `wait ms`,
        // which `Rtx::FrameSamples` carries for the harness and for the game alike so that the two
        // reports can be read against each other.
        //
        // **`collectFrame` and not `finishFrame`**: the frame behind stays on the device while this
        // one is placed, which is what keeps the device busy from one trace to the next — 0.9 ms
        // of a 6 ms frame on the ship, in `.notes/bench.txt`. What comes back is the frame before
        // it, so the bench row carries a report two frames behind this frame's wall time.
        // `Check::FramesOverlap` is what says the ring still holds two.
        //
        // **Timed as well as waited for**, because the wait is not the whole of it: the ring then
        // reads the device's counters and its timestamps and destroys what that frame was the last
        // to read, and none of that is in the figure the device reports.
        const std::chrono::steady_clock::time_point finishing = std::chrono::steady_clock::now();
        report.mResult = mRenderer->collectFrame();
        report.mSpend.at(Rtx::Timing::Finish) = Rtx::since(finishing, std::chrono::steady_clock::now());
    }

    void RtxRenderer::handOver(const SceneFrame& frame, FrameReport& report)
    {
        // Placed, appended or rebuilt — the decision, and the describing a rebuild needs, are the
        // harness's too and are written once (`Rtx::SceneUploader`).
        const std::chrono::steady_clock::time_point handing = std::chrono::steady_clock::now();
        const Rtx::SceneUpload handed = mMirror.hand(*mRenderer, frame.mImages, report.mSpend);
        report.mSpend.at(Rtx::Timing::Place) = Rtx::since(handing, std::chrono::steady_clock::now());
        report.mRebuilt = handed.mKind == Rtx::SceneUpload::Kind::Rebuilt;

        mHasScene = true;

        if (report.mRebuilt)
            Log(Debug::Info) << "Ray tracing built " << mMirror.getScene().meshes().getRows().size() << " meshes into "
                             << mWalked.mFound.mInstances << " instances with " << mWalked.mFound.mLights << " lights, "
                             << mWalked.mFound.mDeformed << " of them deforming, and skipped "
                             << mWalked.mFound.mSkippedUnknown << " it cannot read";

        mUnreadable += handed.mUnreadable;

        if (handed.mUnreadable > 0)
            Log(Debug::Warning) << "Ray tracing could not read " << handed.mUnreadable << " of " << handed.mDescribed
                                << " textures and drew them grey — a live graph holds textures that were never files";
    }

    std::optional<Rtx::Shaders::VisibilityConstants> RtxRenderer::aim(const SceneFrame& frame)
    {
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
        try
        {
            Rtx::Shaders::VisibilityConstants constants = Rtx::makeCameraFromView(frame.mCamera.getViewMatrix(),
                frame.mEye.mFieldOfView, extents.mRenderWidth, extents.mRenderHeight, sNear, Rtx::sFarPlane);

            // What the game decided the eye sees, read where the rasterizer reads it.
            constants.mRayMask = rayMaskOf(getViewMask());
            return constants;
        }
        catch (const Rtx::Error& what)
        {
            // **Asked of the builder rather than tested for here**: a test here would be a copy of
            // the builder's contract with two places to be right. Reported once, because a camera
            // nobody filled in and a real defect look identical from here until it is said how
            // often it happens.
            if (!mComplained)
            {
                mComplained = true;
                Log(Debug::Warning) << "Ray tracing skipped a frame: " << what.what();
            }

            return std::nullopt;
        }
    }

    void RtxRenderer::trace(const SceneFrame& frame, Rtx::Shaders::VisibilityConstants constants, FrameReport& report)
    {
        const Rtx::WorldReading read
            = mMirror.readWorld(frame.mWorld, static_cast<float>(frame.mWhen.getSimulationTime()));

        const float exposureBias = Rtx::describeWorld(read, mFogDrift, constants);

        // **What the sampler and the jitter are walked by, and leaving it at zero is a bug with two
        // faces.** The bounce samples the same point every frame, so nothing ever converges; and the
        // upscaler, which jitters whatever it is told, is handed the same sub-pixel offset every
        // frame and reconstructs from one sample taken repeatedly. The harness had exactly this, and
        // it cost a picture that looked plausible and carried none of the detail it was paying for.
        //
        // **The stop's own count where a run is being made, and the game's frame number
        // otherwise.** `RtxRun::getSampleFrame` says why: a measured run has to walk the same
        // sequence twice, and a game's frame number carries the loading screen's frames with it.
        constants.mFrame = mRun.getSampleFrame().value_or(static_cast<std::uint32_t>(mFrame));

        // **The schedule's and not the profile's**, because a warm-up is not averaged in — a picture
        // of a half-built cell in the sum is what `RtxRun::getAccumulated` exists to keep out.
        const std::uint32_t accumulated = mRun.getAccumulated();

        // **Both hosts light the world by the profile's rules.** `Rtx::makeCameraFromView` names
        // every field it fills and leaves the rest value-initialised, and `texturing.glsl`
        // short-circuits on a `mDelight` of nought, handing the trace Bethesda's textures with
        // their painted lighting still in them.
        constants.mDelight = mProfile.mDelight;
        constants.mShowAlbedo = mProfile.mShowAlbedo ? 1u : 0u;

        // **The bias is carried rather than worked out here**, because a room is the exception to
        // the rule that would derive it — `Rtx::Skylight::mExposureBias`. Whichever light this cell
        // got settled it, and a second derivation at the frame is a second place to get the
        // exception wrong.
        //
        // **Timed, because a profiler cannot read it.** The record and the submit are almost
        // entirely inside the driver, which carries no frame pointer, so perf attributes what they
        // cost to an address with no caller. `Rtx::Timing::Trace` says what the row is for.
        const std::chrono::steady_clock::time_point tracing = std::chrono::steady_clock::now();

        report.mReconstruction = mRenderer->renderFrame(
            constants, Rtx::FrameOptions::forFrame(mProfile, accumulated, mClock.getStatedStep(), exposureBias));

        // **The whole frame, measured between one trace and the next.** Everything the game does
        // in between is in it — update, cull, this — which is what a player feels and what the
        // wait on the device on its own cannot say.
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        const std::optional<double> since = mSpan.enter(now);
        report.mSpend.at(Rtx::Timing::Trace) = Rtx::since(tracing, now);
        report.mSpend.at(Rtx::Timing::Present) = mSpan.takePresent();

        if (since.has_value())
        {
            report.mFrameMs = *since;
            report.mWalked = mWalked;
            report.mUnreadableTextures = mUnreadable;

            if (report.mResult.has_value())
                mRun.frame(describeContext(), report);

            // **Every frame and not the ones the device answered for**, because what this reads is
            // the wall between two traces and the device's answer is not part of it. Once a
            // second, which is how often `Rtx::FrameRate` closes a line — and the window is asked
            // then whether anybody can see it, rather than a copy of that being kept here.
            if (const std::string_view title = mSpeed.addFrame(*since);
                !title.empty() && (SDL_GetWindowFlags(mWindow) & SDL_WINDOW_HIDDEN) == 0)
                SDL_SetWindowTitle(mWindow, title.data());
        }

        // **Counted where it is summed**, because `finishFrame` answers nothing until a frame it
        // put in flight comes back. Counting every frame instead divided the total by frames that
        // had contributed nothing to it, so the average read low by a factor nobody could see.
        if (report.mResult.has_value() && mSpeed.addWait(report.mResult->mWaitMs))
        {
            const Rtx::FrameExtents extents = mRenderer->getExtents();
            const Rtx::SceneDesc& scene = mMirror.getScene();

            // **The emitters among it, because they are the half a placement count does not carry.**
            // Sprites are not instances and never enter that number, so a cell whose every flame,
            // brazier and raindrop had stopped read exactly like one whose emitters were running.
            Log(Debug::Info) << "Ray tracing: waited " << mSpeed.getWaitMs()
                             << " ms a frame for the device over the last " << mSpeed.getFrames() << ", tracing "
                             << scene.placements().getPlacedCount() << " instances and " << scene.emitters().size()
                             << " emitters holding " << scene.sprites().size() << " sprites at " << extents.mRenderWidth
                             << "x" << extents.mRenderHeight << ", reconstructed by "
                             << Rtx::sDenoiserNames.name(report.mReconstruction.mDenoiser) << " to "
                             << extents.mOutputWidth << "x" << extents.mOutputHeight;
        }
    }
}
