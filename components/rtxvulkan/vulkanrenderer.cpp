#include "vulkanrenderer.hpp"

#include <bit>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <ratio>
#include <span>
#include <string>
#include <utility>

#include <osg/Vec2f>
#include <vulkan/vulkan_core.h>

#include <components/rtx/camera.hpp>
#include <components/rtx/error.hpp>
#include <components/rtx/frameimage.hpp>
#include <components/rtx/memoryreport.hpp>
#include <components/rtx/reconstruction.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/shaders/visibility.h>
#include <components/rtx/slot.hpp>
#include <components/rtx/texturedata.hpp>
#include <components/rtx/upscale.hpp>
#include <components/rtx/wavespectrum.hpp>
#include <components/sdlutil/vsyncmode.hpp>

#include "devicescene.hpp"
#include "gbuffer.hpp"
#include "graphicspipeline.hpp"
#include "graveyard.hpp"
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
        /// Under what the launches together take to make, in milliseconds, they were not compiled.
        /// Each launch reports its own time, and they are made in parallel, so the sum is thread
        /// time: forty seconds on this box for a compile, the least of them a second, against
        /// seven hundred milliseconds handed back from the driver's cache and seventy from the
        /// fork's own blob. The floor sits eight times off either side.
        constexpr double sLaunchCompileFloorMs = 5000.0;

        /// One half float, as the number it stands for. By bits, where the test harness spells the
        /// same conversion out by arithmetic, so that each derivation checks the other.
        float fromHalf(std::uint16_t bits)
        {
            const std::uint32_t sign = static_cast<std::uint32_t>(bits & 0x8000u) << 16;
            const std::uint32_t exponent = (bits >> 10) & 0x1fu;
            const std::uint32_t mantissa = bits & 0x3ffu;

            if (exponent == 31)
                return std::bit_cast<float>(sign | 0x7f800000u | (mantissa << 13));

            // A subnormal half is its mantissa times 2^-24, and the float it widens to is normal —
            // so the shuffle below cannot make it and a multiply is what does.
            if (exponent == 0)
            {
                const float magnitude = static_cast<float>(mantissa) * 0x1p-24f;

                return (bits & 0x8000u) != 0 ? -magnitude : magnitude;
            }

            // Bias 15 to bias 127, and ten mantissa bits to twenty-three.
            return std::bit_cast<float>(sign | ((exponent + 112u) << 23) | (mantissa << 13));
        }

        /// The instance a window needs, which is the headless one plus whatever SDL asks for.
        std::vector<const char*> surfaceExtensionsFor(const RendererOptions& options)
        {
            if (options.mWindow == nullptr)
                return {};

            return Presenter::getInstanceExtensions(options.mWindow);
        }

        /// A swapchain is the only thing presenting adds to the device.
        std::vector<const char*> deviceExtensionsFor(const RendererOptions& options)
        {
            if (options.mWindow == nullptr)
                return {};

            return { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        }

        /// How much wider the arms' image plane is than the eye's, per axis —
        /// `VisibilityConstants::mArmsSpread`.
        osg::Vec2f armsSpreadOf(const Shaders::VisibilityConstants& frame)
        {
            return osg::Vec2f(frame.mArms.mRight.length() / frame.mCamera.mRight.length(),
                frame.mArms.mUp.length() / frame.mCamera.mUp.length());
        }
    }

    VulkanRenderer::VulkanRenderer(const RendererOptions& options)
        : mInstance(options.mValidation, surfaceExtensionsFor(options))
        , mDevice(mInstance, PhysicalDevice::select(mInstance.getHandle()),
              PipelineCacheSpec{ .mDirectory = options.mCacheDirectory, .mShaderDirectory = options.mShaderDirectory },
              deviceExtensionsFor(options))
        , mCountHits(options.mCountHits)
        , mProfile(options.mProfile)
        , mChannelLayout(GBuffer::describeLayout(mDevice))
        , mFogVolumeLayout(FogVolume::describeLayout(mDevice))
        , mTextureLayout(TextureArray::describeLayout(mDevice))
        , mPass(mDevice, options.mShaderDirectory, mTextureLayout, mChannelLayout, mFogVolumeLayout, mCountHits)
        , mComposite(mDevice, options.mShaderDirectory)
        , mSpriteBin(mDevice, options.mShaderDirectory)
        , mSpriteShade(mDevice, options.mShaderDirectory)
        // `SAMPLED` because an upscaler samples what it is handed, and one bit short of that is a
        // black frame nothing reports. See `GBuffer`, which carries it for the same reason.
        // `TRANSFER_SRC` because `readComposite` copies this out: it is the frame a measurement is
        // taken on, where `readPixels` gives the one a display would show.
        , mFrame(mDevice, mChannelLayout, mFogVolumeLayout, mPass, mComposite, mSpriteBin, mSpriteShade,
              options.mShaderDirectory,
              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "colour")
        , mView(mDevice, mChannelLayout, mFogVolumeLayout, mPass, mComposite, mSpriteBin, mSpriteShade,
              options.mShaderDirectory, VK_IMAGE_USAGE_STORAGE_BIT, "view colour")
        , mDisplay(mDevice, mPass, mTextureLayout.get(), options.mShaderDirectory, PresentTargets::sFormat)
        , mWaves(mDevice, options.mShaderDirectory)
        , mRipples(mDevice, options.mShaderDirectory)
        , mFog(mDevice)
        , mSkinPass(mDevice, options.mShaderDirectory)
        , mMipChainPass(mDevice, options.mShaderDirectory)
        , mShadingPass(mDevice, options.mShaderDirectory)
        , mSpriteLightPass(mDevice, options.mShaderDirectory)
        , mTexturePasses{ mMipChainPass, mShadingPass, mSpriteLightPass }
        , mGroundPass(mDevice, options.mShaderDirectory, mTextureLayout.get())
        , mNoSprites(Buffer::hostWritten(
              mDevice, 2 * sizeof(std::uint32_t), VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, "no sprites"))
        , mViewCounts(
              Buffer::deviceLocal(mDevice, sizeof(FrameCounts), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "picture counts"))
        , mGuiPass(mDevice, options.mShaderDirectory, PresentTargets::sFormat)
        , mGuiTextures(mDevice)
    {
        // **A launch the driver handed back from a cache of its own is refused where the run
        // refused that cache.** `refuseDriverShaderCache` says why the two codes draw apart; the
        // word it sets is one the driver may not read on every system, and a run measured on
        // the cache's code would be one that never says so. A compile of the launches is seconds
        // on any card that can run them, and a cache hands them back in a few milliseconds, so
        // the floor sits between with room on both sides. The application's own cache is not
        // the driver's: what it hands back is a compile of this fork's, and it counts as none.
        if (driverShaderCacheRefused())
        {
            const std::optional<double> made = mPass.getCompileMs();
            if (made.has_value() && *made < sLaunchCompileFloorMs)
                throw Error(
                    std::format("the driver handed the ray tracing pipelines back from its own shader cache in "
                                "{:.0f} ms, where a compile takes seconds: this run was told to refuse that "
                                "cache and would draw the cache's code rather than this build's. Turn the "
                                "driver's shader cache off in its settings",
                        *made));
        }

        // `SPRITE_LIST_UNBINNED` and a count of nought are both nought.
        mNoSprites.clear();

        // Before the first targets, because what to trace at is its answer and not ours.
        if (mProfile.mUpscaling.mMode != Upscale::Off)
            startUpscaler();

        if (mProfile.mStressOverlapMs > 0.0)
            mStress = std::make_unique<StressPass>(mDevice, options.mShaderDirectory, mProfile.mStressOverlapMs);

        // Before the first targets, because a windowed renderer is sized by its surface rather
        // than by what the caller guessed the window would come up at.
        if (options.mWindow != nullptr)
            mPresenter
                = std::make_unique<Presenter>(mDevice, mInstance.getHandle(), options.mWindow, options.mVerticalSync);

        const VkExtent2D output
            = mPresenter != nullptr ? mPresenter->getExtent() : VkExtent2D{ options.mWidth, options.mHeight };
        createTargets(output.width, output.height);
    }

    VulkanRenderer::~VulkanRenderer()
    {
        // What the interface handed over, before the pool holding it is taken apart. A GUI
        // texture write waits for nothing and rides the next submit this pool makes, and there is
        // no next submit here.
        tearDown("the interface's last writes were not submitted", [&] { mGuiTextures.finish(); });

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

    bool VulkanRenderer::upscaling() const
    {
        return mUpscaler != nullptr && mProfile.mUpscaling.mMode != Upscale::Off;
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
        createTargets(mOutputWidth, mOutputHeight);
    }

    void VulkanRenderer::setSea(const SeaState& sea)
    {
        if (sea == mWaves.getSea())
            return;

        // A frame in flight may still be synthesising from the spectrum this replaces, and so may
        // a picture recorded and not yet carried: `describe` submits the deferred batches itself,
        // after it has destroyed the amplitudes they read.
        drain();
        mWaves.describe(sea);
    }

    void VulkanRenderer::createTargets(std::uint32_t width, std::uint32_t height)
    {
        assert(width > 0 && height > 0);

        mOutputWidth = width;
        mOutputHeight = height;

        // Whatever upscales picks the render size. Asked of the mode and not of the runtime: a
        // runtime that is up because somebody upscaled and then turned it off is kept for the next
        // time, and asking it what to trace at for no upscaling is a question it refuses.
        const VkExtent2D output{ width, height };
        const VkExtent2D render = upscaling() ? mUpscaler->renderSizeFor(output, mProfile.mUpscaling.mMode) : output;
        mFrame.resize(render.width, render.height, mProfile.mRadianceWidth);

        // Two, and interchangeable, because the frame after this one must not rewrite the image
        // the present is still blitting out of. `PresentTargets` is what holds that rule.
        mTargets.resize(mDevice, mOutputWidth, mOutputHeight);

        // A runtime that is up because somebody upscaled and then turned it off keeps nothing
        // but itself: the feature and its image go with the mode.
        if (upscaling())
            mUpscaler->resize(render, output, mProfile.mUpscaling);
        else if (mUpscaler != nullptr)
            mUpscaler->release();

        // Over whatever the frame is by the time the curve maps it, which is the upscaler's
        // output where one runs and the trace's own extent where none does. The same test the frame
        // path makes, because a pyramid built at the other extent is a bloom at the wrong scale.
        const std::uint32_t shownWidth = upscaling() ? mOutputWidth : mFrame.getWidth();
        const std::uint32_t shownHeight = upscaling() ? mOutputHeight : mFrame.getHeight();
        mDisplay.resize(shownWidth, shownHeight);

        // A frame of a different size is not one this one can be reprojected against.
        mPreviousCamera = Shaders::VisibilityConstants{};

        // Dropped rather than resized, because most runs never make one: sixteen bytes a pixel is
        // worth it to the reference mode and nothing to a window. The first averaging frame asks.
        mSum = Image();
    }

    std::string VulkanRenderer::describeDevice() const
    {
        std::string report = "loader:            Vulkan " + versionString(mInstance.getApiVersion()) + '\n'
            + "validation:        " + (mInstance.getValidationLog() != nullptr ? "on" : "off") + '\n'
            + "debug utils:       " + (mInstance.hasDebugUtils() ? "on" : "off") + '\n';

        report += mDevice.getPhysicalDevice().describe();

        report += "\nDLSS Ray Reconstruction: " + describeUpscaling(mDevice, mInstance.getHandle()) + '\n';

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

    const std::unique_ptr<DeviceScene>& VulkanRenderer::slotAt(const SceneSlot slot) const
    {
        if (slot.isWorld())
            return mWorld;

        assert(slot.getViewIndex() < mViewScenes.size() && "a scene slot nothing was given");
        return mViewScenes[slot.getViewIndex()];
    }

    std::unique_ptr<DeviceScene>& VulkanRenderer::slotAt(const SceneSlot slot)
    {
        return const_cast<std::unique_ptr<DeviceScene>&>(std::as_const(*this).slotAt(slot));
    }

    const DeviceScene& VulkanRenderer::sceneAt(const SceneSlot slot) const
    {
        const std::unique_ptr<DeviceScene>& held = slotAt(slot);
        assert(held != nullptr && "a scene slot nothing holds");
        return *held;
    }

    DeviceScene& VulkanRenderer::sceneAt(const SceneSlot slot)
    {
        return const_cast<DeviceScene&>(std::as_const(*this).sceneAt(slot));
    }

    VisibilityInputs VulkanRenderer::describeInputs(const DeviceScene& held, const TraceChain& chain,
        const std::uint32_t rayMask, const Image& shown, const Buffer& counts, const FrameSlot traceSlot) const
    {
        return VisibilityInputs{
            .mScene = held.getAcceleration().getTopLevel(),
            .mBuffers = &held.getBuffers(),
            .mSlot = held.getSlot(),
            .mTraceSlot = traceSlot,
            .mChannels = &chain.getChannels(),
            .mCounts = &counts,
            .mIndexBlocks = held.getAcceleration().getIndexBlocks(),
            .mTextures = held.getTextures(),
            .mWaves = &mWaves,
            .mRipples = &mRipples,
            .mFog = &mFog,
            .mFogVolume = &chain.getFogVolume(),
            .mSpriteList = (rayMask & Shaders::MASK_PARTICLE) != 0 ? 0 : mNoSprites.addressFor(),
            .mShown = &shown,
            .mSunGlare = &mDisplay.getGlareCounts(),
            .mWater = held.getCounts().mWater > 0,
        };
    }

    Shaders::VisibilityConstants VulkanRenderer::sampleCamera(const Shaders::VisibilityConstants& camera,
        const DeviceScene& scene, const Reconstruction& reconstruction,
        const Shaders::VisibilityConstants* previous) const
    {
        Shaders::VisibilityConstants sampled = camera;

        // Where in the pixel this frame samples. Filled here rather than by the caller because the
        // sequence belongs to the frame index, which is the renderer's to walk.
        if (reconstruction.mJitter)
            sampled.mCamera.mJitter = haltonJitter(camera.mFrame);

        // The arms' eye samples where the world's does, or the two halves of one frame would be
        // reconstructed from two grids.
        sampled.mArms.mJitter = sampled.mCamera.mJitter;
        sampled.mArmsSpread = armsSpreadOf(camera);

        // The scene's answer and not the camera's, for the reason `VisibilityInputs::mWater` is one:
        // a cell with no cloud in it has nothing for the medium walk to find, wherever it is looked
        // at from. The arms are the scene's and the camera's both: a map draws none.
        const InstanceCounts& counts = scene.getCounts();
        sampled.mMediumInFrame = counts.mMedium > 0 ? 1 : 0;
        sampled.mAdditiveInFrame = counts.mAdditive > 0 ? 1 : 0;
        sampled.mArmsInFrame = counts.mFirstPerson > 0 && (camera.mRayMask & Shaders::MASK_FIRST_PERSON) != 0 ? 1 : 0;

        // The one subtraction of two world points, and it happens here. Two camera positions a
        // step apart subtract exactly in a float; the same difference taken on the device, between
        // coordinates six figures long, would be rounding. A picture has no last frame and keeps
        // the caller's nothing.
        if (previous != nullptr)
        {
            sampled.mCameraMotion = camera.mOrigin - previous->mOrigin;
            sampled.mPreviousForward = previous->mCamera.mForward;
            sampled.mPreviousRight = previous->mCamera.mRight;
            sampled.mPreviousUp = previous->mCamera.mUp;
        }

        return sampled;
    }

    void VulkanRenderer::setScene(const SceneSlot slot, const SceneDesc& scene, std::span<const TextureData> textures)
    {
        std::unique_ptr<DeviceScene>& held = slotAt(slot);

        // Nothing may be in flight over what is about to go. A rebuild is a load, and a load
        // waits: for a picture recorded against the old scene and not yet carried, for the frames
        // tracing it, and for a placement the frame being recorded may have submitted without a
        // fence of its own.
        drain();

        // Torn down before anything is built, so a second scene does not hold two of everything at
        // once — a cell's structures and textures are most of what this renderer occupies.
        held.reset();

        if (slot.isWorld())
        {
            // The reports of a world that has gone are dropped, and this is the only place they
            // are. A caller counts the frames it drew, so an arrival or a resize keeps its
            // reports and hands them over as it asks; a new world is that count starting again, and
            // a report from before it would answer the next question with the wrong frame.
            mRing.dropReports();

            // A sum over one scene means nothing over the next, so it goes back with the scene
            // rather than being carried empty into one it cannot describe. Neither does a motion
            // vector, which would point at where something stood in a world that is no longer there.
            mSum = Image();
            mPreviousCamera = Shaders::VisibilityConstants{};
        }

        // One submit for the whole cell, asked of the queue once at the flush below — by hand
        // rather than left to the destructor, so a submit that fails throws out of here instead
        // of being logged on the way past.
        Batch setup(mDevice.getPool());
        held = std::make_unique<DeviceScene>(
            mDevice, setup, mTextureLayout, mSkinPass, mTexturePasses, mGroundPass, scene, textures);
        setup.flush();

        if (slot.isWorld())
            held->readStats(mStats);
    }

    void VulkanRenderer::extendScene(const SceneSlot slot, const SceneDesc& scene, std::span<const TextureData> arrived)
    {
        DeviceScene& held = sceneAt(slot);

        // An arrival does not wait for the frames in flight: what arrives is written on the queue,
        // behind whatever a frame in flight still reads of the room it was given, and the writes
        // end in the barrier `orderStagedWrites` records. It opens the frame it lands in, because
        // `beginFrame` clears the timer and a zone opened before it would be forgotten.
        GpuTimer* timer = nullptr;
        if (slot.isWorld())
            timer = &mRing.begin().mTimer;

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
        const std::unique_ptr<DeviceScene>& held = slotAt(slot);
        return held != nullptr ? held->describe() : SceneHeld{};
    }

    void VulkanRenderer::dropTextures(const SceneSlot slot, std::span<const Index> textures)
    {
        // Before there is a scene at all, which is one that swept before it was ever handed over.
        // There is nothing holding the images to destroy.
        if (DeviceScene* const held = slotAt(slot).get(); held != nullptr)
            held->dropTextures(textures);
    }

    void VulkanRenderer::placeScene(const SceneSlot slot, const SceneDesc& scene)
    {
        DeviceScene& held = sceneAt(slot);

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
        mGuiTextures.finish();
        mPresenter->setVerticalSync(mode);
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
            // Asked before anything is drained, because `fitToWindow` calls this every settled
            // frame. Same reason as the destructor's: remaking a swapchain waits the device idle
            // and frees the blit's buffers, and a batch handed over is sitting beside them waiting
            // for a submit. What that costs where no rebuild follows is `Presenter::wantsResize`.
            if (mPresenter->wantsResize(VkExtent2D{ width, height }))
            {
                mGuiTextures.finish();
                mPresenter->rebuild(VkExtent2D{ width, height });
            }

            // What the swapchain came back with, not what was asked for. A surface clamps to
            // what it can do, and targets sized to the request would then be blitted through a
            // scale nobody chose.
            const VkExtent2D shown = mPresenter->getExtent();
            width = shown.width;
            height = shown.height;
        }

        if (width == mOutputWidth && height == mOutputHeight)
            return;

        // The images about to be replaced may still be in flight.
        drain();
        createTargets(width, height);
    }

    GuiSlot VulkanRenderer::addGuiTexture(std::uint32_t width, std::uint32_t height)
    {
        return mGuiTextures.add(width, height);
    }

    std::span<std::uint8_t> VulkanRenderer::lendGuiTexture(const GuiSlot texture, const GuiRegion& region)
    {
        return mGuiTextures.lend(texture, region);
    }

    void VulkanRenderer::sendGuiTexture(const GuiSlot texture)
    {
        mGuiTextures.send(texture);
    }

    void VulkanRenderer::dropGuiTexture(const GuiSlot texture)
    {
        mGuiTextures.drop(texture);
    }

    void VulkanRenderer::drawGui(std::span<const GuiVertex> vertices, std::span<const GuiBatch> batches)
    {
        assert(mTargets.isOpen());

        if (vertices.empty() || batches.empty())
            return;

        // The interface drawn two frames ago drew out of this slot, and the vertices carry the
        // submit that bound them: that passed is what says they may be written over.
        FrameRecord& gui = mRing.slotOf(mGuiFrame);
        if (!gui.mGuiVertices.isIdle())
            gui.mGuiVertices.waitIdle("the interface drawn two frames ago");

        // After the wait, which collected, and before anything is handed over: this frame's submit is the first
        // that says every draw with a texture given back has finished, and the staging turns on
        // the same signal.
        mGuiTextures.startFrame();

        growTo(gui.mGuiVertices, mDevice, BufferKind::HostWritten, vertices.size_bytes(),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "gui vertices");
        gui.mGuiVertices.write(vertices);

        // Named by hand, because a vertex buffer is bound by handle and not handed out as an
        // address or a descriptor.
        gui.mGuiVertices.nameForNext();

        mGuiDraws.clear();
        mGuiDraws.reserve(batches.size());
        for (const GuiBatch& batch : batches)
        {
            const VkImageView view = mGuiTextures.getView(batch.mTexture);
            assert(view != VK_NULL_HANDLE && "a batch names a texture this renderer does not hold");

            // A slot nothing holds would be a null descriptor, which is undefined rather than
            // blank. The assert above is where a caller finds out; a release build drops the batch.
            if (view != VK_NULL_HANDLE)
                mGuiDraws.push_back(GuiDraw{ view, batch.mFirstVertex, batch.mVertexCount,
                    batch.mBlend == GuiBlend::Additive ? Blend::Additive : Blend::Over });
        }

        // Its own submit, after the frame's, and not waited for. The GUI is collected once the
        // world has been drawn and there is nothing to gain by holding the frame open for it; the
        // queue draws it after the frame, the present blits after both, and the wait is for the
        // vertices alone.
        const VkCommandBuffer commands = gui.mGuiCommands;
        mDevice.getPool().begin(commands);
        claimTarget().transition(commands, Use::sComputeWrite, Use::sColourAttachment);

        mGuiPass.record(commands, mTargets.current(), gui.mGuiVertices.getHandle(), mGuiDraws);

        // Back where everything else expects it: the presenter blits out of `GENERAL` and so
        // does a read back.
        mTargets.current().transition(commands,
            ImageUse{ VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT },
            Use::sAnyGeneralRead);

        mDevice.getPool().submit(commands);
        ++mGuiFrame;
    }

    bool VulkanRenderer::presentFrame()
    {
        assert(mPresenter != nullptr && "presentFrame on a renderer that was given no window");
        assert(mTargets.isOpen());

        const bool shown = mPresenter->present(mTargets.current());
        mTargets.presented();
        return shown;
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
            .mOutputWidth = mOutputWidth,
            .mOutputHeight = mOutputHeight,
        };
    }

    Reconstruction VulkanRenderer::renderFrame(const Shaders::VisibilityConstants& camera, const FrameOptions& options)
    {
        assert(mWorld != nullptr && "renderFrame before setScene");
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

        // The count is an atomic sum over the frame, so it starts each one at nothing — and it is
        // not started at all where the trace was specialized to write nothing into it, which is the
        // other half of taking the counter out of the game: the atomic went with `COUNT_HITS`, and
        // this is the write a frame that never reads it was still paying for.
        if (mCountHits)
            frame.mHitCount.writable<FrameCounts>(0, 1).front() = FrameCounts{};

        // What reconstructs this frame, decided once and by one rule. Every switch below reads
        // this rather than working the interaction out again; the same value goes back in the frame
        // result, so what a run reports and what it did are one answer.
        const Reconstruction reconstruction = Reconstruction::resolve(mProfile.mUpscaling, options.mReconstruction);
        frame.mReconstruction = reconstruction;

        Shaders::VisibilityConstants sampled = sampleCamera(camera, *mWorld, reconstruction, &mPreviousCamera);

        // The puffs are composited over the reconstruction where something upscales, and over the
        // trace's own composite where nothing does. Named before the trace, because the set that
        // carries it is pushed for every launch.
        const VisibilityInputs inputs = describeInputs(*mWorld, mFrame, camera.mRayMask,
            upscaling() ? mUpscaler->getOutput() : mFrame.getColour(), frame.mHitCount, mRing.getRecordingSlot());

        // Made by the first frame that averages, and that frame is the one that fills it.
        const bool fresh = options.mAccumulate > 0 && mSum.isEmpty();
        if (fresh)
            mSum = Image(mDevice, mFrame.getWidth(), mFrame.getHeight(), VK_FORMAT_R32G32B32A32_SFLOAT,
                VK_IMAGE_USAGE_STORAGE_BIT, "sum");

        // A history is worthless after a jump no motion vector can describe: walking through a
        // door once left the previous camera intact and a reprojection fetched one room onto
        // another. Asking is what spends the denoiser's signal, and `historyRead` says whether a
        // pass did, so a `resetHistory` before an unfiltered frame waits for the frame that can act
        // on it.
        const bool basisLost = mPreviousCamera.mCamera.mForward.length2() <= 0.0f;
        const bool airLost = mAirStale || basisLost;
        const bool historyLost = mDenoiserStale || basisLost;

        GpuTimer& timer = frame.mTimer;
        const VkCommandBuffer commands = frame.mWorld.mCommands;
        mDevice.getPool().begin(commands);

        // The glare fader's query starts the frame at nothing, ahead of the trace that counts.
        mDisplay.beginGlare(commands);

        // What walked through the water, stepped before the trace reads it and only where the
        // world stands in a sea: one field under every picture of this frame, anchored where the
        // step left it. A frame with no sea leaves the tiles as they were and stands no field.
        if (inputs.mWater)
        {
            mRipples.record(commands, mRing.getRecordingSlot(), options.mRipples,
                osg::Vec2f(camera.mOrigin.x(), camera.mOrigin.y()), static_cast<double>(camera.mSkyTime), &timer);
            sampled.mRippleOrigin = mRipples.getOrigin();
            sampled.mRippleExtent = RipplePass::getExtent();
        }

        // The first write needs no contents and nothing to wait on; every one after reads what
        // the last left, which the queue orders and does not make visible.
        if (!mSum.isEmpty())
            mSum.transition(commands,
                ImageUse{ fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                    fresh ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    fresh ? 0 : VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT },
                Use::sComputeReadWrite);

        // Ray Reconstruction is itself the denoiser, and handing it a frame the wavelet already
        // blurred is asking it to recover what was thrown away — which is why `resolve` never
        // answers with both.
        const bool filtering = reconstruction.filtered();
        bool historyRead = filtering;

        const GBuffer& channels = mFrame.getChannels();
        Image& target = claimTarget();

        const Image* shown = &mFrame.record(commands,
            TraceRecording{
                .mInputs = inputs,
                .mBuffers = &mWorld->getBuffers(),
                .mAsked = camera,
                .mSampled = sampled,
                .mTarget = &target,
                .mSum = mSum.isEmpty() ? nullptr : &mSum,
                .mAccumulate = options.mAccumulate,
                .mAirLost = airLost,
                .mHistoryLost = historyLost,
                .mFilter = filtering,
                .mTimer = &timer,
            });

        if (upscaling())
        {
            historyRead = true;
            timer.open(commands, "upscale");
            shown = &mUpscaler->record(commands,
                UpscaleInputs{
                    .mColour = mFrame.getColour(),
                    .mDiffuseAlbedo = channels.get(Channel::Albedo),
                    .mSpecularAlbedo = channels.get(Channel::Specular),
                    .mNormalRoughness = channels.get(Channel::Guide),
                    .mDepth = channels.get(Channel::Depth),
                    .mMotion = channels.get(Channel::Motion),
                    .mReflectionMotion = channels.get(Channel::ReflectionMotion),
                    .mJitter = sampled.mCamera.mJitter,
                    .mFrameDeltaMs = sinceLastMs,
                    .mReset = historyLost,
                });
            timer.close(commands);
        }

        // The rest of the frame, over the reconstruction where something upscales — which the
        // upscaler left where the puffs want it — and over the trace's own composite where nothing
        // does. The whole of the frame is the picture, which is the output's extent either way.
        assert(shown == inputs.mShown && "the puffs composited over a frame the set does not name");
        assert(shown->getWidth() == mOutputWidth && shown->getHeight() == mOutputHeight);
        if (!upscaling())
            shown->transition(commands, Use::sAnyGeneralRead, Use::sTraceReadWrite);

        // The third thing that reads a lost history, and the only one that reads it on every
        // frame: the eye has no past to adapt from either.
        const float sinceLastSeconds = 0.001f * sinceLastMs;
        Display::Exposure exposure
            = Display::Measured{ .mSeconds = sinceLastSeconds, .mReset = historyLost, .mBias = options.mExposureBias };
        if (options.mExposure.has_value())
            exposure = Display::Fixed{ *options.mExposure };
        else
            historyRead = true;

        mDisplay.record(commands,
            Display{
                .mShown = *shown,
                .mExtent = VkExtent2D{ mOutputWidth, mOutputHeight },
                .mInputs = inputs,
                .mSampled = sampled,
                .mTarget = target,
                .mExposure = exposure,
                .mBloom = true,
                .mGlare = Display::Glare{ .mSeconds = sinceLastSeconds, .mReset = historyLost },
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
            mStress->record(commands, timer);

        // Submitted and not waited for: `finishFrame` or `collectFrame` brings the count and the
        // report back a frame or two late. A wait's access scope is the device's, so the counters
        // need a dependency of their own, recorded here after every pass that could have added to
        // them.
        if (mCountHits)
            frame.mHitCount.orderForHostRead(commands);

        mRing.submit(frame);

        // What the next frame reprojects against, and the camera as the caller gave it: a jitter is
        // where inside a pixel this frame sampled, not where the eye was.
        mPreviousCamera = camera;

        // The air's is spent here and unconditionally, because the trace above always ran. A
        // frame that filled the volume was told; a frame with no volume to fill has nothing to keep
        // a stale flag for, and holding it would zero the basis — and so every motion vector — for
        // as long as the player stayed indoors.
        mAirStale = false;

        if (historyRead)
            mDenoiserStale = false;

        return reconstruction;
    }

    SceneSlot VulkanRenderer::addViewScene()
    {
        // Empty until `setScene` fills it: a slot is a name, and the scene arrives with the first
        // description.
        if (const Index taken = mFreeViewScenes.take(); taken != sNoIndex)
        {
            mViewScenes[taken] = nullptr;
            return SceneSlot::view(taken);
        }

        mViewScenes.push_back(nullptr);
        return SceneSlot::view(static_cast<std::uint32_t>(mViewScenes.size() - 1));
    }

    void VulkanRenderer::dropViewScene(const SceneSlot scene)
    {
        assert(scene.getViewIndex() < mViewScenes.size() && "a scene nothing handed out");
        assert(!mFreeViewScenes.isFree(scene.getViewIndex()) && "a scene given back twice");

        // Buried and not drained: a picture of it recorded this frame and not yet carried rides the
        // next submit, and so does the last placement's refit, so the scene goes once the timeline
        // has passed that submit and no sooner, which is the graveyard's rule for everything. The
        // graveyard frees a scene after every structure, because a structure the scene retired
        // gives its room back to a storage the scene owns. A drain here idled the whole device
        // every time the inventory closed.
        mDevice.getGraveyard().bury(std::shared_ptr<void>(std::move(mViewScenes[scene.getViewIndex()])));
        mFreeViewScenes.free(scene.getViewIndex());
    }

    void VulkanRenderer::growViewTargets(std::uint32_t width, std::uint32_t height)
    {
        if (mView.holds(width, height))
            return;

        // The one drain a picture still pays, and only the first picture of a new size pays it:
        // the whole device, because what `grow` replaces is destroyed and not buried, and a
        // destruction asserts the queue idle.
        drain();

        mView.grow(width, height, mProfile.mRadianceWidth);

        mViewTarget = Image(mDevice, mView.getWidth(), mView.getHeight(), PresentTargets::sFormat,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "view target");
    }

    void VulkanRenderer::traceGuiTexture(
        const GuiSlot texture, const Shaders::VisibilityConstants& camera, const GuiTraceOptions& options)
    {
        const bool held = mGuiTextures.holds(texture);
        assert(held && "a trace into a slot nothing holds");

        // The camera's own extent is how much of the texture the picture fills.
        const VkExtent2D extent{ camera.mCamera.mWidth, camera.mCamera.mHeight };
        if (!held || extent.width == 0 || extent.height == 0)
            return;

        growViewTargets(extent.width, extent.height);

        DeviceScene& traced = sceneAt(options.mScene);

        const VisibilityInputs inputs
            = describeInputs(traced, mView, camera.mRayMask, mView.getColour(), mViewCounts, FrameSlot{});

        // Nothing reconstructs a picture, so nothing jitters it, and it has no frame before it.
        Shaders::VisibilityConstants sampled = sampleCamera(camera, traced, Reconstruction{}, nullptr);

        // The world's ripple field where the picture is of the world, which is the one place it
        // could have a wake in it; a subject of its own stands in no sea.
        if (options.mScene.isWorld())
        {
            sampled.mRippleOrigin = mRipples.getOrigin();
            sampled.mRippleExtent = RipplePass::getExtent();
        }

        // Recorded into a batch that rides the next submit, and waited for by nobody here: the
        // next placement of this scene waits for what its tables say, and what it writes nothing
        // else reads. Not counted and not timed, because the hit count and the report are the
        // frame's.
        Batch trace(mDevice.getPool());
        {
            const VkCommandBuffer commands = trace.getCommands();

            // A doll and a map tile are one frame with no frame before them, so every history says
            // so: the accumulator becomes a pass-through handing on the largest variance there is,
            // which is what tells the cascade to filter as widely as it can.
            mView.record(commands,
                TraceRecording{
                    .mInputs = inputs,
                    .mBuffers = &traced.getBuffers(),
                    .mAsked = camera,
                    .mSampled = sampled,
                    .mTarget = &mViewTarget,
                });

            // The puffs over the picture — a torch's flame in a doll's hand is a sprite — and the
            // curve, and nothing else: a picture is measured off nothing, mapped with no share and
            // spread by no lens, because a map tile is a diagram and the same armour must be the
            // same brightness in two windows.
            mView.getColour().transition(commands, Use::sAnyGeneralRead, Use::sTraceReadWrite);
            mDisplay.record(commands,
                Display{
                    .mShown = mView.getColour(),
                    .mExtent = extent,
                    .mInputs = inputs,
                    .mSampled = sampled,
                    .mTarget = mViewTarget,
                    .mExposure = Display::Picture{},
                });

            mViewTarget.transition(commands, Use::sComputeWrite, Use::sCopyRead);

            // Borrowed rather than transitioned. Where a GUI texture rests between writes is
            // `GuiTextures`' to say, and a caller that said it here had to keep a barrier's scope in
            // step with the commands below — which it did not.
            mGuiTextures.writeWith(texture, commands, [&](const Image& into, VkImageLayout layout) {
                assert(extent.width <= into.getWidth() && extent.height <= into.getHeight());

                // Cleared whole and then covered in part, and only where the picture does not
                // cover it all: what the trace fills is as much of the texture as the widget is
                // currently wide, and the rest has to be the clear colour rather than what a wider
                // picture left there the last time this was drawn.
                // Both are transfer writes to the same image and nothing orders two of those, so
                // the clear is left as what the copy meets.
                if (extent.width < into.getWidth() || extent.height < into.getHeight())
                    into.clear(commands, Use::sTransferWrite,
                        VkClearColorValue{
                            .float32 = { options.mClear[0], options.mClear[1], options.mClear[2], options.mClear[3] } },
                        Use::sCopyWrite);

                assert(layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                    && "a texture lent in another layout than a copy takes");
                mViewTarget.copyTo(commands, into, layout, extent);
            });

            if (options.mReadBack)
                mGuiTextures.readBackWith(texture, commands);
        }
        trace.defer();

        // The value the batch rides: the next submit this pool makes, whichever that is. The
        // tables it reads were named the same value as they were handed out above.
        traced.notePictureRide(traced.getSlot(), mDevice.getTimeline().getNext());
    }

    bool VulkanRenderer::takeGuiCopy(const GuiSlot texture, const std::span<std::uint8_t> into)
    {
        return mGuiTextures.takeCopy(texture, into);
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
        mGuiTextures.read(texture, pixels);
    }

    void VulkanRenderer::readPixels(std::vector<std::uint8_t>& pixels)
    {
        assert(mTargets.isOpen());

        // The frame that was finished, not the one the next will be written into. A present has
        // already swapped those two; with no window nothing presents, nothing swaps, and the frame
        // just written is still the one `mTarget` names.
        const Image& frame = mTargets.lastPresented() != nullptr ? *mTargets.lastPresented() : mTargets.current();
        frame.read(VK_IMAGE_LAYOUT_GENERAL, pixels);
    }

    void VulkanRenderer::readChannel(const Channel channel, std::vector<float>& values)
    {
        assert(mFrame.isBuilt());

        // One lookup and not a switch of eleven arms. A channel is its binding, and the buffer
        // is indexed by it.
        readImage(mFrame.getChannels().get(channel), values);
    }

    void VulkanRenderer::readComposite(std::vector<float>& values)
    {
        assert(mFrame.isBuilt());
        readImage(mFrame.getColour(), values);
    }

    void VulkanRenderer::readImage(const Image& image, std::vector<float>& values)
    {
        std::vector<std::uint8_t> bytes;
        image.read(VK_IMAGE_LAYOUT_GENERAL, bytes);

        // Every format the renderer reads back is named, and one that is not is a throw rather
        // than a `memcpy`, which is how the motion channels came back as pairs of halves the day
        // they narrowed. Tested on the format, because several macros across three headers name
        // each of these.
        switch (image.getFormat())
        {
            case VK_FORMAT_R16G16_SFLOAT:
            case VK_FORMAT_R16G16B16A16_SFLOAT:
                values.resize(bytes.size() / sizeof(std::uint16_t));
                for (std::size_t at = 0; at < values.size(); ++at)
                {
                    std::uint16_t half = 0;
                    std::memcpy(&half, bytes.data() + at * sizeof(half), sizeof(half));
                    values[at] = fromHalf(half);
                }

                return;

            case VK_FORMAT_R32_SFLOAT:
            case VK_FORMAT_R32G32_SFLOAT:
            case VK_FORMAT_R32G32B32A32_SFLOAT:
                values.resize(bytes.size() / sizeof(float));
                std::memcpy(values.data(), bytes.data(), bytes.size());

                return;

            default:
                throw Error("no float decode is recorded for this image format");
        }
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
