#include "vulkanrenderer.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <ratio>
#include <span>
#include <string>

#include <osg/Vec2f>
#include <vulkan/vulkan_core.h>

#include <components/rtx/framedigest.hpp>
#include <components/rtx/frameimage.hpp>
#include <components/rtx/frameoptions.hpp>
#include <components/rtx/framesampling.hpp>
#include <components/rtx/memoryreport.hpp>
#include <components/rtx/reconstruction.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/digest.h>
#include <components/rtx/shaders/visibility.h>
#include <components/rtx/slot.hpp>
#include <components/rtx/texturedata.hpp>
#include <components/rtx/upscale.hpp>
#include <components/rtx/wavespectrum.hpp>
#include <components/sdlutil/vsyncmode.hpp>

#include "devicescene.hpp"
#include "gbuffer.hpp"
#include "image.hpp"
#include "imageuse.hpp"
#include "memory.hpp"
#include "physicaldevice.hpp"
#include "pipelinecache.hpp"
#include "placing.hpp"
#include "presenter.hpp"
#include "requirements.hpp"
#include "result.hpp"
#include "texture.hpp"
#include "timeline.hpp"
#include "tracerecording.hpp"
#include "upscaler.hpp"
#include "validation.hpp"
#include "visibilitypass.hpp"

namespace Rtx
{
    namespace
    {
        /// The instance a window needs, which is the headless one plus whatever SDL asks for — the
        /// surface among it, which is what tells the device to take a swapchain.
        std::vector<const char*> surfaceExtensionsFor(const RendererOptions& options)
        {
            if (options.mWindow == nullptr)
                return {};

            return Presenter::getInstanceExtensions(options.mWindow);
        }
    }

    VulkanRenderer::VulkanRenderer(const RendererOptions& options)
        : mInstance(options.mValidation, surfaceExtensionsFor(options))
        , mDevice(mInstance, PhysicalDevice::select(mInstance.getHandle()),
              PipelineCacheSpec{ .mDirectory = options.mCacheDirectory, .mShaderDirectory = options.mShaderDirectory })
        , mCounting(options.mCounting)
        , mProfile(options.mProfile)
        , mReadsCounts(mCounting || mProfile.mStressOverlapMs > 0.0)
        , mChannelLayout(GBuffer::describeLayout(mDevice))
        , mFogVolumeLayout(FogVolume::describeLayout(mDevice))
        , mTextureLayout(TextureArray::describeLayout(mDevice))
        , mPass(mDevice, options.mShaderDirectory, mTextureLayout, mChannelLayout, mFogVolumeLayout, mCounting,
              mProfile.mSpecializeLaunches, mProfile.mReorder)
        , mComposite(mDevice, options.mShaderDirectory)
        , mSpriteBin(mDevice, options.mShaderDirectory)
        , mSpriteShade(mDevice, options.mShaderDirectory)
        , mAccumulate(mDevice, options.mShaderDirectory)
        , mFilter(mDevice, options.mShaderDirectory)
        // `SAMPLED` because an upscaler samples what it is handed, and one bit short of that is a
        // black frame nothing reports. See `GBuffer`, which carries it for the same reason.
        // `TRANSFER_SRC` because `readComposite` copies this out: it is the frame a measurement is
        // taken on, where `readPixels` gives the one a display would show.
        , mFrame(mDevice, describeTracePasses(),
              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "colour")
        , mDisplay(mDevice, mPass, mTextureLayout.get(), options.mShaderDirectory, PresentTargets::sFormat)
        , mDigest(mDevice, options.mShaderDirectory)
        , mMedia(mDevice, options.mShaderDirectory)
        , mSkinPass(mDevice, options.mShaderDirectory)
        , mMipChainPass(mDevice, options.mShaderDirectory)
        , mShadingPass(mDevice, options.mShaderDirectory)
        , mSpriteLightPass(mDevice, options.mShaderDirectory)
        , mTexturePasses{ mMipChainPass, mShadingPass, mSpriteLightPass }
        , mGroundPass(mDevice, options.mShaderDirectory, mTextureLayout.get())
        , mScenes(mDevice)
        , mGui(mDevice, options.mShaderDirectory, PresentTargets::sFormat)
        , mPictures(mDevice, describeTracePasses(), mMedia, mDisplay, mGui.getTextures())
    {
        mDevice.getMemory().limitBudget(options.mMemoryBudget);

        // Before the first targets, because what to trace at is its answer and not ours.
        if (mProfile.mUpscaling.mMode != Upscale::Off)
            startUpscaler();

        if (mProfile.mStressOverlapMs > 0.0)
            mStress = std::make_unique<StressPass>(mDevice, options.mShaderDirectory, mProfile.mStressOverlapMs);

        // Before the first targets, because a windowed renderer is sized by its surface rather
        // than by what the caller guessed the window would come up at.
        if (options.mWindow != nullptr)
            mPresenter = std::make_unique<Presenter>(
                mDevice, mInstance, options.mWindow, options.mVerticalSync, options.mPacing);

        const VkExtent2D output
            = mPresenter != nullptr ? mPresenter->getExtent() : VkExtent2D{ options.mWidth, options.mHeight };
        createTargets(output.width, output.height);
    }

    VulkanRenderer::~VulkanRenderer()
    {
        // What the interface handed over, before the pool holding it is taken apart. A GUI
        // texture write waits for nothing and rides the next submit this pool makes, and there is
        // no next submit here.
        tearDown("the interface's last writes were not submitted", [&] { mGui.getTextures().finish(); });

        // Every frame in flight, and the presenter's last blit, before anything they name goes.
        tearDown("the device would not finish before the renderer was taken apart", [&] { mDevice.waitIdle(); });

        // Before the scenes below it, which own the storage the buried rooms are rooms in.
        mDevice.collectIdle();
    }

    void VulkanRenderer::startUpscaler()
    {
        if (mUpscaler != nullptr)
            return;

        // A quarter of a second, which is why it waits to be wanted. Bringing the runtime up
        // loads the feature libraries; a player who never upscales should not spend that at every
        // start, and one who turns it on in the menu spends it once.
        mUpscaler = makeUpscaler(mDevice, mInstance.getHandle());
    }

    void VulkanRenderer::resetHistory()
    {
        mFrame.resetHistory();
        mDisplay.resetHistory();
        mUpscalerStale = true;
        mMedia.resetRipples();
    }

    void VulkanRenderer::drain()
    {
        mDevice.getPool().finishDeferred();
        mRing.finishAll();
        mDevice.waitIdle();

        // Everything buried, the scenes given back included, before what replaces them is made.
        mDevice.collectIdle();
    }

    void VulkanRenderer::setUpscale(Upscale upscale)
    {
        if (upscale == mProfile.mUpscaling.mMode)
            return;

        // Before anything is torn down, so a mode this machine cannot reach leaves the renderer
        // drawing exactly as it was rather than half way between two of them.
        if (upscale != Upscale::Off)
            startUpscaler();

        mProfile.mUpscaling.mMode = upscale;

        // What is about to be replaced may still be in flight.
        drain();

        const VkExtent2D output = mTargets.getExtent();
        createTargets(output.width, output.height);
    }

    void VulkanRenderer::setSea(const SeaState& sea)
    {
        if (sea == mMedia.getSea())
            return;

        // A frame in flight may still be synthesising from the spectrum this replaces, and so may
        // a picture recorded and not yet carried: `describe` submits the deferred batches itself,
        // after it has destroyed the amplitudes they read.
        drain();
        mMedia.describeSea(sea);
    }

    void VulkanRenderer::createTargets(std::uint32_t width, std::uint32_t height)
    {
        assert(width > 0 && height > 0);

        // Whatever upscales picks the render size. Asked of the mode and not of the runtime: a
        // runtime that is up because somebody upscaled and then turned it off is kept for the next
        // time, and asking it what to trace at for no upscaling is a question it refuses.
        const VkExtent2D output{ width, height };
        const VkExtent2D render = upscaling() ? mUpscaler->renderSizeFor(output, mProfile.mUpscaling.mMode) : output;
        mFrame.resize(render.width, render.height, mProfile.mRadianceWidth);

        // Two, and interchangeable, because the frame after this one must not rewrite the image
        // the present is still blitting out of. `PresentTargets` is what holds that rule.
        mTargets.resize(mDevice, width, height);

        // A runtime that is up because somebody upscaled and then turned it off keeps nothing
        // but itself: the feature and its image go with the mode.
        if (upscaling())
            mUpscaler->resize(render, output, mProfile.mUpscaling);
        else if (mUpscaler != nullptr)
            mUpscaler->release();

        // Over the output extent, which is what the frame is by the time the curve maps it: the
        // upscaler's output where one runs, and the trace itself, at the same size, where none does.
        mDisplay.resize(width, height);

        // A frame of a different size is not one this one can be reprojected against.
        mPreviousCamera = Shaders::VisibilityConstants{};
    }

    std::string VulkanRenderer::describeDevice() const
    {
        std::string report = "loader:            Vulkan " + versionString(mInstance.getApiVersion()) + '\n'
            + "validation:        " + (mInstance.getValidationLog() != nullptr ? "on" : "off") + '\n'
            + "debug utils:       " + (mInstance.hasDebugUtils() ? "on" : "off") + '\n';

        report += mDevice.getPhysicalDevice().describe();

        report += "\nDLSS Ray Reconstruction: " + describeUpscaling(mDevice, mInstance.getHandle()) + '\n';

        // The device's half of the driver's pacing; the surface's half is a window's to answer,
        // and this verb has none.
        report += "frame pacing:      "
            + std::string(mDevice.hasLatencyPacing() ? "the driver paces (VK_NV_low_latency2, presentId)"
                                                     : "the host paces its own frames")
            + '\n';

        // Reaching here is the part that proves the rest: the device resolved every entry point the
        // required extensions promise, and a driver advertising one it cannot dispatch fails before
        // this line rather than at the first frame that needed it.
        report += "\nlogical device and every required entry point: ok\n";

        return report;
    }

    bool VulkanRenderer::isValidating() const
    {
        // The log exists only where the layer was found, so this answers "loaded" and not "asked".
        return mInstance.getValidationLog() != nullptr;
    }

    void VulkanRenderer::setScene(const SceneSlot slot, const SceneDesc& scene, std::span<const TextureData> textures)
    {
        // Buried, so a picture recorded against the old scene and not yet carried keeps it until
        // the submit that carries the picture has run.
        mScenes.bury(slot);

        if (slot.isWorld())
        {
            // Nothing may be in flight over what is about to go. A new world is a load, and a load
            // waits: for the frames tracing the old one, and for a placement the frame being
            // recorded may have submitted without a fence of its own. The drain frees what was
            // just buried, so a second world does not hold two of everything at once — a cell's
            // structures and textures are most of what this renderer occupies.
            drain();

            // The reports of a world that has gone are dropped, and this is the only place they
            // are. A caller counts the frames it drew, so an arrival or a resize keeps its
            // reports and hands them over as it asks; a new world is that count starting again, and
            // a report from before it would answer the next question with the wrong frame.
            mRing.dropReports();

            // A sum over one scene means nothing over the next, so it goes back with the scene
            // rather than being carried empty into one it cannot describe. Neither does a motion
            // vector, which would point at where something stood in a world that is no longer there.
            mFrame.dropSum();
            mPreviousCamera = Shaders::VisibilityConstants{};
        }

        // What the device has room for is decided below, against what it says now.
        mDevice.getMemory().refreshBudget(mDevice.getTimeline().getNext());

        Batch setup(mDevice.getPool());
        DeviceScene& held = mScenes.hold(slot,
            std::make_unique<DeviceScene>(
                mDevice, setup, mTextureLayout, mSkinPass, mTexturePasses, mGroundPass, scene, textures));

        // A picture's scene rides the next submit, as an arrival does: its placement and its trace
        // are deferred behind it, and the barrier every upload and build ends in orders them. The
        // world's is one submit for the whole cell, asked of the queue here — by hand rather than
        // left to the destructor, so a submit that fails throws out of here instead of being logged
        // on the way past.
        if (!slot.isWorld())
        {
            setup.defer();
            return;
        }

        setup.flush();
        held.readStats(mStats);
        mMedia.keepRipples(scene);
    }

    void VulkanRenderer::extendScene(const SceneSlot slot, const SceneDesc& scene, std::span<const TextureData> arrived)
    {
        DeviceScene& held = mScenes.at(slot);

        // An arrival does not wait for the frames in flight: what arrives is written on the queue,
        // behind whatever a frame in flight still reads of the room it was given, and the writes
        // end in the barrier `orderStagedWrites` records. It opens the frame it lands in, because
        // `beginFrame` clears the timer and a zone opened before it would be forgotten.
        GpuTimer* timer = nullptr;
        if (slot.isWorld())
            timer = &mRing.begin().mTimer;

        mDevice.getMemory().refreshBudget(mDevice.getTimeline().getNext());

        Batch setup(mDevice.getPool());
        held.extend(setup, scene, arrived, timer);

        // Deferred to the placement's submit: `placeScene` submits this ahead of the refit and the
        // top level, and the barrier every upload and build ends in orders them, so a composite
        // landing costs no submit, fence or wait of its own.
        setup.defer();

        // Always, because the top level names every instance and an arrival changed the list. It is
        // rebuilt every frame regardless, so an arrival costs it nothing.
        placeScene(slot, scene);

        // The history is kept. Nothing was renumbered, so what the last frame resolved still
        // describes the same surfaces — and throwing it away is a visible flash every time an actor
        // walks into view with a texture nobody has worn yet.
        if (slot.isWorld())
            held.readStats(mStats);
    }

    SceneHeld VulkanRenderer::describeHeld(const SceneSlot slot) const
    {
        const DeviceScene* held = mScenes.find(slot);
        return held != nullptr ? held->describe() : SceneHeld{};
    }

    std::span<const Refusal> VulkanRenderer::getRefusals(const SceneSlot slot) const
    {
        return mScenes.at(slot).getRefusals();
    }

    void VulkanRenderer::dropTextures(const SceneSlot slot, std::span<const Index> textures)
    {
        // Before there is a scene at all, which is one that swept before it was ever handed over.
        // There is nothing holding the images to destroy.
        if (DeviceScene* const held = mScenes.find(slot); held != nullptr)
            held->dropTextures(textures);
    }

    void VulkanRenderer::placeScene(const SceneSlot slot, const SceneDesc& scene)
    {
        DeviceScene& held = mScenes.at(slot);

        // The copy this placement writes is the one the last frame did not trace. The other copy
        // and not a parity of its own, because a frame need not place.
        const FrameSlot into = held.getSlot().next();

        // A picture of this copy recorded and carried by nothing yet is carried first, for what
        // `DeviceScene::pictureRides` says. Three placements of one scene inside one frame is the
        // only way here, which a game never takes.
        if (held.pictureRides(into, mDevice.getTimeline().getNext()))
            mDevice.getPool().finishDeferred();

        // Whatever last read or wrote this copy on the queue is waited for here, and each table
        // says what that was: the frame before last's trace, which `collectFrame` has usually
        // waited out already, so this is a comparison; an arrival's pose over the first copy,
        // carried by a placement's submit; a picture carried by the interface's own. Where the
        // device is behind, this is the right place for the CPU to stand still.
        held.finishReads(into);

        // A picture inside the interface is placed into a batch that rides the next submit, neither
        // timed nor opening the frame's report; the trace that follows is deferred the same way, and
        // the barrier `place` ends in orders the pair.
        if (!slot.isWorld())
        {
            Batch placement(mDevice.getPool());
            held.place(scene,
                Placing{
                    .mCommands = placement.getCommands(),
                    .mSlot = into,
                });
            placement.defer();
            held.placed(into);
            return;
        }

        // A placement opens the frame, and every placement before a trace joins it. The frame's
        // report starts here and not at the trace: placing the world is the refit and the top level,
        // and a report that began at `renderFrame` would leave them out.
        FrameRecord& frame = mRing.begin();

        // The placement's own submit, without a wait. The frame's trace, later on the queue,
        // covers this submit too. Nothing recorded is nothing submitted, which is every frame of a
        // standing camera in an empty place.
        const VkCommandBuffer placement = mRing.takePlaceCommands(frame);
        mDevice.getPool().begin(placement);

        if (held.place(scene,
                Placing{
                    .mCommands = placement,
                    .mSlot = into,
                    .mTimer = &frame.mTimer,
                }))
            mDevice.getPool().submit(placement);
        else
            mDevice.getPool().end(placement);

        held.placed(into);

        held.readPlacedStats(mStats);
        mMedia.keepRipples(scene);
    }

    MemoryReport VulkanRenderer::getMemoryReport() const
    {
        return mDevice.getMemory().report();
    }

    void VulkanRenderer::setVerticalSync(SDLUtil::VSyncMode mode)
    {
        // Headless: `shot`, `bench` and `check` present to nothing, and a run with no surface has
        // no refresh to meet.
        if (mPresenter == nullptr)
            return;

        // A handed-over batch is submitted first, exactly as a resize does.
        mGui.getTextures().finish();
        mPresenter->setVerticalSync(mode);
    }

    bool VulkanRenderer::pacesFrames() const
    {
        return mPresenter != nullptr && mPresenter->pacesFrames();
    }

    void VulkanRenderer::setPacing(const Pacing& pacing)
    {
        if (mPresenter == nullptr)
            return;

        // A handed-over batch is submitted first, as a vertical sync change does: the present mode
        // may move with the pacing, and a rebuild frees what a batch may be sitting beside.
        mGui.getTextures().finish();
        mPresenter->setPacing(pacing);
    }

    void VulkanRenderer::awaitFrame()
    {
        if (mPresenter != nullptr)
            mPresenter->awaitFrame();
    }

    void VulkanRenderer::endSimulation(const bool flash)
    {
        assert(!mRing.isOpen() && "a frame the world was placed in was neither traced nor skipped");

        if (mPresenter != nullptr)
            mPresenter->endSimulation(flash);
    }

    std::optional<LatencyReport> VulkanRenderer::describeLatency() const
    {
        if (mPresenter == nullptr)
            return std::nullopt;

        return mPresenter->describeLatency();
    }

    void VulkanRenderer::skipFrame()
    {
        if (mRing.isOpen())
            mRing.skip();
    }

    TracePasses VulkanRenderer::describeTracePasses() const
    {
        return TracePasses{
            .mChannels = mChannelLayout,
            .mFog = mFogVolumeLayout,
            .mVisibility = mPass,
            .mComposite = mComposite,
            .mSpriteBin = mSpriteBin,
            .mSpriteShade = mSpriteShade,
            .mAccumulate = mAccumulate,
            .mFilter = mFilter,
        };
    }

    std::uint64_t VulkanRenderer::getFrameCount() const
    {
        return mRing.getRecording();
    }

    std::optional<FrameResult> VulkanRenderer::finishFrame()
    {
        return mRing.collect();
    }

    std::optional<FrameResult> VulkanRenderer::collectFrame()
    {
        return mRing.collectFinished();
    }

    void VulkanRenderer::resize(std::uint32_t width, std::uint32_t height)
    {
        if (mPresenter != nullptr)
        {
            // Asked before anything is drained, because `RtxWindow::fit` calls this every settled
            // frame. Same reason as the destructor's: remaking a swapchain waits the device idle
            // and frees the blit's buffers, and a batch handed over is sitting beside them waiting
            // for a submit. What that costs where no rebuild follows is `Presenter::wantsResize`.
            if (mPresenter->wantsResize(VkExtent2D{ width, height }))
            {
                mGui.getTextures().finish();
                mPresenter->rebuild(VkExtent2D{ width, height });
            }

            // What the swapchain came back with, not what was asked for. A surface clamps to
            // what it can do, and targets sized to the request would then be blitted through a
            // scale nobody chose.
            const VkExtent2D shown = mPresenter->getExtent();
            width = shown.width;
            height = shown.height;
        }

        if (width == mTargets.getExtent().width && height == mTargets.getExtent().height)
            return;

        // The images about to be replaced may still be in flight.
        drain();
        createTargets(width, height);
    }

    GuiSlot VulkanRenderer::addGuiTexture(std::uint32_t width, std::uint32_t height)
    {
        return mGui.getTextures().add(width, height);
    }

    std::span<std::uint8_t> VulkanRenderer::lendGuiTexture(const GuiSlot texture, const GuiRegion& region)
    {
        return mGui.getTextures().lend(texture, region);
    }

    void VulkanRenderer::sendGuiTexture(const GuiSlot texture)
    {
        mGui.getTextures().send(texture);
    }

    void VulkanRenderer::dropGuiTexture(const GuiSlot texture)
    {
        mGui.getTextures().drop(texture);
    }

    void VulkanRenderer::drawGui(std::span<const GuiVertex> vertices, std::span<const GuiBatch> batches)
    {
        assert(mTargets.isOpen());

        if (vertices.empty() || batches.empty())
            return;

        // After the frame's submit, and not waited for. The GUI is collected once the world has
        // been drawn and there is nothing to gain by holding the frame open for it; the queue draws
        // it after the frame, and the present blits after both.
        mGui.draw(vertices, batches, claimTarget());
    }

    void VulkanRenderer::presentFrame()
    {
        assert(mPresenter != nullptr && "presentFrame on a renderer that was given no window");
        assert(mTargets.isOpen());

        mPresenter->present(mTargets.current());
        mTargets.presented();
    }

    Image& VulkanRenderer::claimTarget()
    {
        // A present's blit outlives the call that queued it — under FIFO it waits until the
        // presentation engine has let that swapchain image go — and no barrier's source scope
        // reaches across a submit, so the target is waited for by the stamp the blit left on it.
        return mTargets.claim([](const Image& target) { target.waitIdle("the blit that last read this frame"); });
    }

    FrameExtents VulkanRenderer::getExtents() const
    {
        return FrameExtents{
            .mRenderWidth = mFrame.getWidth(),
            .mRenderHeight = mFrame.getHeight(),
            .mOutputWidth = mTargets.getExtent().width,
            .mOutputHeight = mTargets.getExtent().height,
        };
    }

    Reconstruction VulkanRenderer::renderFrame(const Shaders::VisibilityConstants& camera, const FrameOptions& options)
    {
        const DeviceScene* const held = mScenes.find(SceneSlot::world());
        assert(held != nullptr && "renderFrame before setScene");
        const DeviceScene& world = *held;
        assert(camera.mCamera.mWidth == mFrame.getWidth() && camera.mCamera.mHeight == mFrame.getHeight()
            && "the camera has to be built for the render extent; ask getExtents");

        // Coverage and an upscaler do not meet: NGX writes the upscaled image itself and was never
        // given `pInAlpha`, so a picture that stops where nothing was hit is `traceGuiTexture`'s.
        assert((camera.mTransparentBackground == 0 || mProfile.mUpscaling.mMode == Upscale::Off)
            && "a frame that stops where nothing was hit belongs to traceGuiTexture, which does not upscale");

        // The frame `placeScene` opened, or a new one where nothing was placed.
        FrameRecord& frame = mRing.begin();

        // How long since the last one, which the upscaler tunes its denoising against and the
        // exposure adapts over. Off the wall only where the caller has no schedule, because a run
        // that reads the clock for this is a run whose pictures depend on how fast it drew them.
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        const float sinceLastMs = options.mSinceLast.has_value()
            ? *options.mSinceLast * 1000.0f
            : (mLastFrameAt.has_value() ? std::chrono::duration<float, std::milli>(now - *mLastFrameAt).count() : 0.0f);
        mLastFrameAt = now;

        // The miss count is an atomic sum over the frame, so the block starts each one at nothing
        // — and it is not started at all where nothing reads it back, which is the other half of
        // taking the counters out of the game: the atomics went with `COUNTING`, and this is the
        // write a frame that never reads it was still paying for.
        if (mReadsCounts)
            frame.mCounts.writable<Shaders::FrameCounts>(0, 1).front() = Shaders::FrameCounts{};

        // What reconstructs this frame, decided once and by one rule. Every switch below reads
        // this rather than working the interaction out again; the same value goes back in the frame
        // result, so what a run reports and what it did are one answer.
        const Reconstruction reconstruction = Reconstruction::resolve(
            mProfile.mUpscaling, options.mReconstruction.value_or(mProfile.mReconstruction), getExtents());
        frame.mReconstruction = reconstruction;

        Shaders::VisibilityConstants sampled
            = sampleFrame(camera, options, mProfile, reconstruction, world.getCounts(), &mPreviousCamera);

        // The launch the misses are counted against, which is the traced extent and not the shown one.
        frame.mCountedRays = mCounting ? sampled.mCamera.mWidth * sampled.mCamera.mHeight : 0u;

        // The puffs are composited over the reconstruction where something upscales, and over the
        // trace's own composite where nothing does. Named before the trace, because the set that
        // carries it is pushed for every launch.
        const VisibilityInputs inputs
            = mMedia.describe(world, camera, reconstruction.upscaled() ? mUpscaler->getOutput() : mFrame.getColour(),
                frame.mCounts, mDisplay.getGlareCounts(), mRing.getRecordingSlot());

        // Every history is worthless after a jump no motion vector can describe: walking through a
        // door once left the previous camera intact and a reprojection fetched one room onto
        // another. What `resetHistory` said is each history's own, spent by the frame that reads
        // that history, so a reset before an unfiltered frame waits for the frame that filters.
        const bool basisLost = mPreviousCamera.mCamera.mForward.length2() <= 0.0f;
        const bool upscalerLost = reconstruction.upscaled() && (basisLost || mUpscalerStale);

        GpuTimer& timer = frame.mTimer;
        const VkCommandBuffer commands = frame.mWorld.mCommands;
        mDevice.getPool().begin(commands);

        // The glare fader's query starts the frame at nothing, ahead of the trace that counts.
        mDisplay.beginGlare(commands);

        // What walked through the water, stepped before the trace reads it and only where the
        // world stands in a sea: one field under every picture of this frame, anchored where the
        // step left it. A frame with no sea leaves the tiles as they were and stands no field.
        if (inputs.mSea)
        {
            mMedia.stepRipples(commands, mRing.getRecordingSlot(), osg::Vec2f(camera.mOrigin.x(), camera.mOrigin.y()),
                options.mSkySeconds, &timer);
            mMedia.placeRipples(sampled);
        }

        Image& target = claimTarget();

        const TraceResult traced = mFrame.record(commands,
            TraceRecording{
                .mInputs = inputs,
                .mAsked = camera,
                .mSampled = sampled,
                .mTarget = &target,
                .mAccumulate = options.mAccumulate,
                .mPastLost = basisLost,
                // Ray Reconstruction is itself the denoiser, and handing it a frame the wavelet
                // already blurred is asking it to recover what was thrown away — which is why
                // `resolve` never answers with both.
                .mFilter = reconstruction.filtered(),
                .mTimer = &timer,
            });
        const GBuffer& channels = *traced.mInputs.mChannels;

        // Before anything past the trace has a say, and the channels' hand-over is the read this
        // rides on. `FrameDigest` says why the picture is not enough.
        if (options.mReadBack)
        {
            std::array<const Image*, Shaders::DIGEST_IMAGES> digested{};
            for (const Channel channel : sEveryChannel)
                digested[bindingOf(channel)] = &channels.get(channel);
            digested[Shaders::DIGEST_COMPOSITE] = &traced.mColour;

            mDigest.record(commands, digested, frame.mDigestLanes, &timer);
            frame.mDigest = FrameDigest{
                .mJitterX = sampled.mCamera.mJitter.x(),
                .mJitterY = sampled.mCamera.mJitter.y(),
                .mFrameDeltaMs = sinceLastMs,
                .mReset = upscalerLost ? 1u : 0u,
            };
        }

        if (reconstruction.upscaled())
        {
            timer.open(commands, "upscale");
            mUpscaler->record(commands,
                UpscaleInputs{
                    .mColour = traced.mColour,
                    .mDiffuseAlbedo = channels.get(Channel::Albedo),
                    .mSpecularAlbedo = channels.get(Channel::Specular),
                    .mNormalRoughness = channels.get(Channel::Guide),
                    .mDepth = channels.get(Channel::Depth),
                    .mMotion = channels.get(Channel::Motion),
                    .mReflectionMotion = channels.get(Channel::ReflectionMotion),
                    .mJitter = sampled.mCamera.mJitter,
                    .mFrameDeltaMs = sinceLastMs,
                    .mReset = upscalerLost,
                });
            timer.close(commands);
            mUpscalerStale = false;
        }

        // The rest of the frame, over the reconstruction where something upscales — which the
        // upscaler left where the puffs want it — and over the trace's own composite where nothing
        // does. The whole of the frame is the picture, which is the output's extent either way.
        if (!reconstruction.upscaled())
            traced.mColour.transition(commands, Use::sAnyGeneralRead, Use::sTraceReadWrite);

        const float sinceLastSeconds = 0.001f * sinceLastMs;
        Display::Exposure exposure
            = Display::Measured{ .mSeconds = sinceLastSeconds, .mReset = basisLost, .mBias = options.mExposureBias };
        if (const ExposureRule rule = options.mExposure.value_or(mProfile.mExposure); rule.has_value())
            exposure = Display::Fixed{ *rule };

        mDisplay.record(commands,
            Display{
                .mTrace = traced,
                .mExtent = mTargets.getExtent(),
                .mSampled = sampled,
                .mTarget = target,
                .mExposure = exposure,
                .mBloom = true,
                .mGlare = Display::Glare{ .mFader = options.mGlare, .mSeconds = sinceLastSeconds, .mReset = basisLost },
                .mDebug = options.mDebug,
                .mDebugVertices = &frame.mDebugVertices,
                .mTimer = &timer,
            });

        // The picture as the curve left it and before the interface, into host memory of the
        // ring's, for the report that comes back with the frame. Grown here and not at the resize,
        // because most frames never ask.
        if (options.mReadBack)
        {
            const VkDeviceSize bytes = target.getReadBytes();
            Buffer& picture = mRing.pictureOf(mRing.getRecording());
            growTo(picture, mDevice, BufferKind::ReadBack, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, "frame readback");
            target.recordRead(commands, Use::sComputeWrite, Use::sComputeWrite, picture);
            frame.mReadBackBytes = bytes;
        }

        // After the picture and inside the frame's trace, so the frame is finished when its value
        // has passed and the hold is the last thing it did.
        if (mStress != nullptr)
            mStress->record(commands, timer, frame.mCounts);

        // Submitted and not waited for: `finishFrame` or `collectFrame` brings the counts and the
        // report back a frame or two late. A wait's access scope is the device's, so the counts
        // need a dependency of their own, recorded here after every pass that could have written
        // them — the hold included.
        if (mReadsCounts)
            frame.mCounts.orderForHostRead(commands);

        mRing.submit(frame);

        // What the next frame reprojects against, and the camera as the caller gave it: a jitter is
        // where inside a pixel this frame sampled, not where the eye was.
        mPreviousCamera = camera;

        return reconstruction;
    }

    SceneSlot VulkanRenderer::addViewScene()
    {
        return mScenes.add();
    }

    void VulkanRenderer::dropViewScene(const SceneSlot scene)
    {
        // Buried and not drained: a drain here idled the whole device every time the inventory
        // closed.
        mScenes.drop(scene);
    }

    void VulkanRenderer::traceGuiTexture(
        const GuiSlot texture, const Shaders::VisibilityConstants& camera, const GuiTraceOptions& options)
    {
        const bool held = mGui.getTextures().holds(texture);
        assert(held && "a trace into a slot nothing holds");

        const VkExtent2D extent{ camera.mCamera.mWidth, camera.mCamera.mHeight };
        if (!held || extent.width == 0 || extent.height == 0)
            return;

        // The one drain a picture still pays, and only the first picture of a new size pays it:
        // the whole device, because what `grow` replaces is destroyed and not buried, and a
        // destruction asserts the queue idle.
        if (!mPictures.holds(extent))
        {
            drain();
            mPictures.grow(extent, mProfile.mRadianceWidth);
        }

        mPictures.trace(texture, camera, options, mScenes.at(options.mScene), mProfile);
    }

    bool VulkanRenderer::takeGuiCopy(const GuiSlot texture, const std::span<std::uint8_t> into)
    {
        return mGui.getTextures().takeCopy(texture, into);
    }

    void VulkanRenderer::finishGuiTraces()
    {
        // The pictures recorded and not yet carried, and then the frames carrying the rest; the
        // frame's own chain is left standing.
        mDevice.getPool().finishDeferred();
        mRing.finishAll();
    }

    void VulkanRenderer::readGuiTexture(const GuiSlot texture, std::vector<std::uint8_t>& pixels)
    {
        mGui.getTextures().read(texture, pixels);
    }

    void VulkanRenderer::readPixels(std::vector<std::uint8_t>& pixels)
    {
        assert(mTargets.isOpen());

        // The frame that was finished, not the one the next will be written into. A present has
        // already swapped those two; with no window nothing presents, nothing swaps, and the frame
        // just written is still the one `mTargets.current()` names.
        const Image& frame = mTargets.lastPresented() != nullptr ? *mTargets.lastPresented() : mTargets.current();
        frame.read(VK_IMAGE_LAYOUT_GENERAL, pixels);
    }

    void VulkanRenderer::readChannel(const Channel channel, std::vector<float>& values)
    {
        assert(mFrame.isBuilt());

        // One lookup and not a switch of eleven arms. A channel is its binding, and the buffer
        // is indexed by it.
        mFrame.getChannels().get(channel).readFloats(VK_IMAGE_LAYOUT_GENERAL, values);
    }

    void VulkanRenderer::readComposite(std::vector<float>& values)
    {
        assert(mFrame.isBuilt());
        mFrame.getColour().readFloats(VK_IMAGE_LAYOUT_GENERAL, values);
    }

    void VulkanRenderer::takeValidationErrors(std::vector<std::string>& errors)
    {
        errors.clear();

        ValidationLog* log = mInstance.getValidationLog();
        if (log == nullptr)
            return;

        log->takeErrorsOnThisThread(errors);
    }
}
