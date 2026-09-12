#include "vulkanrenderer.hpp"

#include <bit>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>

#include <components/rtx/camera.hpp>
#include <components/rtx/error.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/gbuffer.h>

#include "framehistory.hpp"
#include "gbuffer.hpp"
#include "image.hpp"
#include "imageuse.hpp"
#include "memory.hpp"
#include "physicaldevice.hpp"
#include "pipelinecache.hpp"
#include "presenter.hpp"
#include "requirements.hpp"
#include "result.hpp"
#include "sceneacceleration.hpp"
#include "scenebuffers.hpp"
#include "skintables.hpp"
#include "texture.hpp"
#include "tracerecording.hpp"
#include "visibilitypass.hpp"

#ifdef OPENMW_RTX_DLSS
#include "dlss.hpp"
#include "dlsspass.hpp"
#endif

namespace Rtx
{
    namespace
    {
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
        InstanceOptions instanceOptionsFor(const RendererOptions& options)
        {
            InstanceOptions instance = toInstanceOptions(options.mValidation);
            if (options.mWindow != nullptr)
                instance.mSurfaceExtensions = Presenter::getInstanceExtensions(options.mWindow);

            return instance;
        }

        /// A swapchain is the only thing presenting adds to the device.
        std::vector<const char*> deviceExtensionsFor(const RendererOptions& options)
        {
            if (options.mWindow == nullptr)
                return {};

            return { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        }

        /// The display pass's own description of the frame: the trace's basis on the picture's
        /// grid, because `rayAt` divides by the camera's own extent. The jitter goes, because this
        /// pass draws once, and the spread angle comes down with the pixel.
        Shaders::ToneConstants toneFor(const Shaders::VisibilityConstants& frame, std::uint32_t width,
            std::uint32_t height, std::uint32_t tracedWidth, std::uint32_t tracedHeight)
        {
            Shaders::Camera shown = frame.mCamera;
            shown.mJitter = osg::Vec2f();
            shown.mSpreadAngle = frame.mCamera.mSpreadAngle * float(tracedHeight) / float(height);
            shown.mWidth = width;
            shown.mHeight = height;

            return Shaders::ToneConstants{
                .mWidth = width,
                .mHeight = height,
                .mTracedWidth = tracedWidth,
                .mTracedHeight = tracedHeight,
                .mCamera = shown,
                .mStars = frame.mStars,
            };
        }
    }

    VulkanRenderer::VulkanRenderer(const RendererOptions& options)
        : mInstance(instanceOptionsFor(options))
        , mDevice(mInstance, PhysicalDevice::select(mInstance.getHandle()),
              PipelineCacheSpec{ .mDirectory = options.mCacheDirectory, .mShaderDirectory = options.mShaderDirectory },
              deviceExtensionsFor(options))
        , mPool(mDevice)
        , mShaderDirectory(options.mShaderDirectory)
        , mCountHits(options.mCountHits)
        , mCountCrossings(options.mCountCrossings)
        , mUpscaling(options.mUpscaling)
        , mChannelLayout(GBuffer::describeLayout(mDevice))
        , mFogVolumeLayout(FogVolume::describeLayout(mDevice))
        // `SAMPLED` because an upscaler samples what it is handed, and one bit short of that is a
        // black frame nothing reports. See `GBuffer`, which carries it for the same reason.
        // `TRANSFER_SRC` because `FrameImage::Composite` copies this out: it is the frame a measurement
        // is taken on, where `readPixels` gives the one a display would show.
        , mFrame(mDevice, mPool, mChannelLayout, mFogVolumeLayout, options.mShaderDirectory,
              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "colour")
        , mView(mDevice, mPool, mChannelLayout, mFogVolumeLayout, options.mShaderDirectory, VK_IMAGE_USAGE_STORAGE_BIT,
              "view colour")
        , mComposite(mDevice, mPool, options.mShaderDirectory)
        , mBloom(mDevice, options.mShaderDirectory)
        , mWaves(mDevice, mPool, options.mShaderDirectory)
        , mFog(mDevice, mPool)
        , mExposure(mDevice, options.mShaderDirectory)
        , mSkinPass(mDevice, options.mShaderDirectory)
        , mSpriteBin(mDevice, options.mShaderDirectory)
        , mSpriteShade(mDevice, options.mShaderDirectory)
        , mNoSprites(Buffer::hostWritten(mDevice, 2 * sizeof(std::uint32_t), VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT))
        , mViewCounts(Buffer::deviceLocal(mDevice, sizeof(FrameCounts), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))
        , mGuiPass(mDevice, options.mShaderDirectory, PresentTargets::sFormat)
        , mGuiTextures(mDevice, mPool)
    {
        // `SPRITE_LIST_UNBINNED` and a count of nought are both nought.
        mNoSprites.clear();

        // Before the first targets, because what to trace at is its answer and not ours.
        if (mUpscaling.mMode != Upscale::Off)
            startUpscaler();

        // Before the first targets, because a windowed renderer is sized by its surface rather
        // than by what the caller guessed the window would come up at.
        if (options.mWindow != nullptr)
            mPresenter = std::make_unique<Presenter>(mDevice, mInstance.getHandle(), options.mWindow);

        const VkExtent2D output
            = mPresenter != nullptr ? mPresenter->getExtent() : VkExtent2D{ options.mWidth, options.mHeight };
        createTargets(output.width, output.height);
    }

    VulkanRenderer::~VulkanRenderer()
    {
        // **What the interface handed over, before the pool holding it is taken apart.** A GUI
        // texture write waits for nothing and rides the next submit this pool makes; there is no
        // next submit here, and `Presenter`'s destructor resets the pool underneath it.
        tearDown("the interface's last writes were not submitted", [&] { mGuiTextures.finish(); });

        // Every frame in flight, and the presenter's last blit, before anything they name goes.
        tearDown("the device would not finish before the renderer was taken apart", [&] { mDevice.waitIdle(); });

        // Before the scenes below it, which own the storage the buried rooms are rooms in.
        mRing.emptyGraveyards();
    }

    void VulkanRenderer::startUpscaler()
    {
#ifdef OPENMW_RTX_DLSS
        if (mNgx != nullptr)
            return;

        // **A quarter of a second, which is why it waits to be wanted.** Bringing the runtime up
        // loads the feature libraries; a player who never upscales should not spend that at every
        // start, and one who turns it on in the menu spends it once.
        mNgx = std::make_unique<Dlss>(mDevice, mInstance.getHandle());
        if (!mNgx->isAvailable())
        {
            const std::string obstacle = mNgx->getObstacle();

            // Let go of it, so that a machine that gains a driver need not be restarted twice and a
            // second attempt is not refused by the one-runtime-per-process rule.
            mNgx.reset();
            throw Unsupported("DLSS Ray Reconstruction was asked for and " + obstacle);
        }
#else
        // **Named rather than quietly ignored.** A build that cannot upscale and renders at the
        // output size anyway is one whose frame times mean something else entirely.
        throw Unsupported("upscaling was asked for and this build has no DLSS; configure with -DOPENMW_RTX_DLSS=ON");
#endif
    }

    bool VulkanRenderer::upscaling() const
    {
#ifdef OPENMW_RTX_DLSS
        return mNgx != nullptr && mUpscaling.mMode != Upscale::Off;
#else
        return false;
#endif
    }

    void VulkanRenderer::setUpscale(Upscale upscale)
    {
        if (upscale == mUpscaling.mMode)
            return;

        // **Before anything is torn down**, so a mode this machine cannot reach leaves the renderer
        // drawing exactly as it was rather than half way between two of them.
        if (upscale != Upscale::Off)
            startUpscaler();

        mUpscaling.mMode = upscale;

        // The same wait a resize makes, and for the same reason: what is about to be replaced may
        // still be in flight.
        mRing.finishAll();
        mDevice.waitIdle();
        createTargets(mOutputWidth, mOutputHeight);
    }

    void VulkanRenderer::createTargets(std::uint32_t width, std::uint32_t height)
    {
        assert(width > 0 && height > 0);

        mOutputWidth = width;
        mOutputHeight = height;

        // Whatever upscales picks the render size. Asked of the mode and not of the runtime: a
        // runtime that is up because somebody upscaled and then turned it off is kept for the next
        // time, and asking it what to trace at for no upscaling is a question it refuses.
        VkExtent2D render{ width, height };
#ifdef OPENMW_RTX_DLSS
        if (upscaling())
            render = mNgx->getRenderSize(VkExtent2D{ width, height }, mUpscaling.mMode);
#endif
        // **The layer channels only where something upscales**, which is the same test
        // `mLayerCompositedAfter` makes of the shader: Ray Reconstruction is the one reader the
        // trace hands a separate layer to, and a frame nothing upscales composites its own.
        mFrame.resize(render.width, render.height, upscaling());

        // **Two, and interchangeable**, because the frame after this one must not rewrite the image
        // the present is still blitting out of. `PresentTargets` is what holds that rule.
        mTargets.resize(mDevice, mPool, mOutputWidth, mOutputHeight);

#ifdef OPENMW_RTX_DLSS
        // Released before the next is built: the feature holds the network's weights for one pair
        // of resolutions, and the image it writes is sixteen bytes a pixel of the output, so neither
        // is left behind for a mode that may not come back.
        mUpscaler.reset();
        mUpscaled.reset();

        if (upscaling())
        {
            // Half floats, where the trace's own composite is full ones: this one is shown and never
            // summed. The peak linear radiance a frame of this game reaches is under nine, measured
            // over the view suite and a camera pointed at the noon sun, so a half carries it with
            // four orders of magnitude to spare at a step finer than the display's.
            mUpscaled = std::make_unique<Image>(mDevice, mOutputWidth, mOutputHeight, VK_FORMAT_R16G16B16A16_SFLOAT,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, "upscaled");

            // Building uploads the network's weights, which is once per resolution rather than
            // once per frame.
            mPool.submitAndWait([&](VkCommandBuffer commands) {
                mUpscaler = std::make_unique<DlssPass>(*mNgx, commands, render,
                    VkExtent2D{ mOutputWidth, mOutputHeight }, mUpscaling.mMode, mUpscaling.mPreset);
            });
        }
#endif

        // **Over whatever the frame is by the time the curve maps it**, which is the upscaler's
        // output where one runs and the trace's own extent where none does. The same test the frame
        // path makes, because a pyramid built at the other extent is a bloom at the wrong scale.
        const std::uint32_t shownWidth = upscaling() ? mOutputWidth : mFrame.getWidth();
        const std::uint32_t shownHeight = upscaling() ? mOutputHeight : mFrame.getHeight();
        mBloom.resize(shownWidth, shownHeight);

        // A frame of a different size is not one this one can be reprojected against.
        mPreviousCamera = Shaders::VisibilityConstants{};

        // Dropped rather than resized, because most runs never make one: sixteen bytes a pixel is
        // worth it to the reference mode and nothing to a window. The first averaging frame asks.
        mSum.reset();
    }

    std::string VulkanRenderer::describeDevice() const
    {
        std::string report = "loader:            Vulkan " + versionString(mInstance.getApiVersion()) + '\n'
            + "validation:        " + (mInstance.getValidationLog() != nullptr ? "on" : "off") + '\n'
            + "debug utils:       " + (mInstance.hasDebugUtils() ? "on" : "off") + '\n';

        report += mDevice.getPhysicalDevice().describe();

#ifdef OPENMW_RTX_DLSS
        report += "\nDLSS Ray Reconstruction: ";
        try
        {
            // **An answer rather than a runtime**, which is why reporting on a device cannot disturb
            // one: NGX keeps one runtime per process and its shutdown is unconditional, so a `Dlss`
            // built to ask with and let go would end this renderer's the moment it left scope.
            const DlssSupport support = Dlss::probe(mDevice, mInstance.getHandle());
            report += support.mAvailable ? "available\n" : "unavailable, " + support.mObstacle + "\n";
        }
        catch (const Error& error)
        {
            report += std::string("unavailable, ") + error.what() + '\n';
        }
#else
        report += "\nDLSS Ray Reconstruction: not built in; configure with -DOPENMW_RTX_DLSS=ON\n";
#endif

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

    const VulkanRenderer::ViewScene& VulkanRenderer::sceneAt(const SceneSlot slot) const
    {
        if (slot.isWorld())
            return mWorld;

        assert(slot.getViewIndex() < mViewScenes.size() && mViewScenes[slot.getViewIndex()] != nullptr
            && "a scene slot nothing holds");
        return *mViewScenes[slot.getViewIndex()];
    }

    VulkanRenderer::ViewScene& VulkanRenderer::sceneAt(const SceneSlot slot)
    {
        return const_cast<ViewScene&>(std::as_const(*this).sceneAt(slot));
    }

    VisibilityInputs VulkanRenderer::describeInputs(
        const ViewScene& held, const FogVolume* const volume, const std::uint32_t rayMask) const
    {
        return VisibilityInputs{
            .mScene = held.mAcceleration->getTopLevel(),
            .mBuffers = held.mBuffers.get(),
            .mSlot = held.mSlot,
            .mIndexBlocks = held.mAcceleration->getIndexBlocks(),
            .mTextures = held.mTextures->getSet(),
            .mWaves = &mWaves,
            .mFog = &mFog,
            .mFogVolume = volume,
            .mSpriteList = (rayMask & Shaders::MASK_PARTICLE) != 0 ? 0 : mNoSprites.getDeviceAddress(),
            .mWater = held.mAcceleration->getInstanceCounts().mWater > 0,
        };
    }

    Shaders::VisibilityConstants VulkanRenderer::sampleCamera(
        const Shaders::VisibilityConstants& camera, const Reconstruction& reconstruction) const
    {
        Shaders::VisibilityConstants sampled = camera;

        // Where in the pixel this frame samples. Filled here rather than by the caller because the
        // sequence belongs to the frame index, which is the renderer's to walk.
        if (reconstruction.mJitter)
            sampled.mCamera.mJitter = haltonJitter(camera.mFrame);

        // **Only Ray Reconstruction reads the transparency layer**, so only a frame it is about to
        // upscale hands its sprites over. Every other trace in this renderer composites them itself.
        sampled.mLayerCompositedAfter = upscaling() ? 1 : 0;

        // The scene's answer and not the camera's, for the reason `VisibilityInputs::mWater` is one:
        // a cell with no cloud in it has nothing for the medium walk to find, wherever it is looked
        // at from.
        sampled.mMediumInFrame = mWorld.mAcceleration->getInstanceCounts().mMedium > 0 ? 1 : 0;

        // **The one subtraction of two world points, and it happens here.** Two camera positions a
        // step apart subtract exactly in a float; the same difference taken on the device, between
        // coordinates six figures long, would be rounding.
        sampled.mCameraMotion = camera.mOrigin - mPreviousCamera.mOrigin;
        sampled.mPreviousForward = mPreviousCamera.mCamera.mForward;
        sampled.mPreviousRight = mPreviousCamera.mCamera.mRight;
        sampled.mPreviousUp = mPreviousCamera.mCamera.mUp;

        return sampled;
    }

    void VulkanRenderer::setScene(
        const SceneSlot slot, const SceneDesc& scene, std::span<const TextureData> textures, const SeaState& sea)
    {
        ViewScene& held = sceneAt(slot);

        // **Nothing may be in flight over what is about to go.** A rebuild is a load, and a load
        // waits: for a picture recorded against the old scene and not yet carried, for the frames
        // tracing it, and for a placement the frame being recorded may have submitted without a
        // fence of its own.
        mPool.finishDeferred();
        mDevice.waitIdle();
        mRing.finishAll();
        mRing.emptyGraveyards();

        // Torn down before anything is built, so a second scene does not hold two of everything at
        // once — a cell's structures and textures are most of what this renderer occupies. The pass
        // is not among them; see below.
        held.mTextures.reset();
        held.mSkinTables.reset();
        held.mBuffers.reset();
        held.mAcceleration.reset();

        if (slot.isWorld())
        {
            // **The reports of a world that has gone are dropped, and this is the only place they
            // are.** A caller counts the frames it drew, so an arrival or a resize keeps its
            // reports and hands them over as it asks; a new world is that count starting again, and
            // a report from before it would answer the next question with the wrong frame.
            mRing.dropReports();

            // A sum over one scene means nothing over the next, so it goes back with the scene
            // rather than being carried empty into one it cannot describe. Neither does a motion
            // vector, which would point at where something stood in a world that is no longer there.
            mSum.reset();
            mPreviousCamera = Shaders::VisibilityConstants{};
        }

        // The copies are new and alike, so nothing has read either.
        held.mSlot = FrameSlot{};
        held.mReadBy.fill(sNeverRead);

        // Made here for the same reason a frame's are: both of the two below want them, and this is
        // the only place that knows both.
        makeInstanceRecords(scene, held.mRecords);

        // **One submit for the whole cell.** Every structure, every table and every texture is
        // recorded into this and the queue is asked once, at the flush below; a round trip apiece
        // would be hundreds for a town.
        Batch setup(mPool);

        Graveyard& graveyard = mRing.recording().mWorld.mGraveyard;

        // **The world's, because there is one sea and every scene traces it.** A doll and a map tile
        // carry a sea state of their own only because they take the same argument, and letting one
        // of those redraw the spectrum would put the interface's water under the world.
        if (slot.isWorld())
            mWaves.describe(sea, graveyard);

        // **Every scene is traced by two frames at once**, the doll's included: a picture inside the
        // interface rides the frame it was asked on, and the next frame may place it again while
        // that one is still tracing.
        held.mAcceleration = std::make_unique<SceneAcceleration>(mDevice, setup, scene, sFrameSlots);
        held.mBuffers = std::make_unique<SceneBuffers>(mDevice, setup, scene, held.mRecords, sFrameSlots, graveyard);
        held.mSkinTables = std::make_unique<SkinTables>(mDevice, scene, sFrameSlots, graveyard);

        held.mTextures = std::make_unique<TextureArray>(
            mDevice, setup, static_cast<std::uint32_t>(scene.textures().getPaths().size()), textures, graveyard);

        // Built once and kept, because building one compiles every kernel the trace can ever need
        // — 6.3 s on a cold cache, measured. Every texture array declares the same bindless
        // layout, and identically defined layouts are compatible, so the pass belongs to neither
        // scene.
        if (mPass == nullptr)
        {
            mPass = std::make_unique<VisibilityPass>(mDevice, setup, mShaderDirectory, held.mTextures->getLayout(),
                mChannelLayout, mFogVolumeLayout, mCountHits, mCountCrossings);
            mTone = std::make_unique<TonePass>(mDevice, mPool, held.mTextures->getLayout(), mShaderDirectory);
        }

        // **Posed before it is built.** The structures are built over the first copy of the
        // positions, and a skinned body's bind pose is not where the body is; the pass writes the
        // pose into that copy and the build then reads it. The other copy is owed the same pose and
        // takes it on the first placement that writes it.
        mSkinPass.record(setup.getCommands(), scene, FrameSlot{}, *held.mSkinTables, held.mAcceleration->getPoses(),
            held.mBuffers->getNormals(), nullptr);
        held.mAcceleration->build(setup, scene, held.mRecords, graveyard);
        held.mBuiltMeshes = scene.meshes().getRevision();
        held.mBuiltStructure = scene.getStructureRevision();

        // By hand rather than left to the destructor, so a submit that fails throws out of here
        // instead of being logged on the way past.
        setup.flush();

        if (slot.isWorld())
            readStats(held);
    }

    void VulkanRenderer::extendScene(
        const SceneSlot slot, const SceneDesc& scene, std::span<const TextureData> arrived, const SeaState& sea)
    {
        ViewScene& held = sceneAt(slot);
        assert(held.mAcceleration != nullptr && "extendScene before setScene");

        // An arrival does not wait for the frames in flight: what arrives is written into room no
        // frame holds, and the writes end in the barrier `orderStagedWrites` records. It opens
        // the frame it lands in, because `beginFrame` clears the timer and a zone opened before
        // it would be forgotten.
        GpuTimer* timer = nullptr;
        if (slot.isWorld())
            timer = &mRing.begin().mTimer;

        Graveyard& graveyard = mRing.recording().mWorld.mGraveyard;

        Batch setup(mPool);
        held.mTextures->write(setup, arrived, graveyard);

        // The meshes that arrived, and no others: the geometry blocks are appended to rather than
        // replaced, so every address a structure was built from is still its own. The revision and
        // not the count, because a freed slot taken over holds different geometry at the same size.
        if (scene.meshes().getRevision() != held.mBuiltMeshes)
        {
            held.mBuffers->extend(setup, scene, graveyard);
            held.mSkinTables->extend(scene, graveyard);
            held.mAcceleration->extend(setup, scene, graveyard);

            // Posed before it is built, as `setScene` does, into the first copy, which is what the
            // build reads. Untimed, so the frame's report carries one `skin` zone and it is the
            // placement's.
            mSkinPass.record(setup.getCommands(), scene, FrameSlot{}, *held.mSkinTables, held.mAcceleration->getPoses(),
                held.mBuffers->getNormals(), nullptr);
            held.mAcceleration->buildArrived(setup, scene, timer, graveyard);
            held.mBuiltMeshes = scene.meshes().getRevision();
        }

        // Deferred to the placement's submit: `placeScene` submits this ahead of the refit and the
        // top level, and the barrier every upload and build ends in orders them, so a composite
        // landing costs no submit, fence or wait of its own.
        setup.defer();

        held.mBuiltStructure = scene.getStructureRevision();

        // Always, because the top level names every instance and an arrival changed the list. It is
        // rebuilt every frame regardless, so an arrival costs it nothing.
        placeScene(slot, scene, sea);

        // **The history is kept.** Nothing was renumbered, so what the last frame resolved still
        // describes the same surfaces — and throwing it away is a visible flash every time an actor
        // walks into view with a texture nobody has worn yet.
        if (slot.isWorld())
            readStats(held);
    }

    SceneHeld VulkanRenderer::describeHeld(const SceneSlot slot) const
    {
        const ViewScene& held = sceneAt(slot);
        return SceneHeld{
            .mBuilt = held.mAcceleration != nullptr,
            .mStructureRevision = held.mBuiltStructure,
            .mTextureCount = held.mTextures == nullptr ? 0 : held.mTextures->getCount(),
        };
    }

    void VulkanRenderer::dropTextures(const SceneSlot slot, std::span<const Index> textures)
    {
        ViewScene& held = sceneAt(slot);

        // Before there is an array at all, which is a scene that swept before it was ever handed
        // over. There is nothing holding the images to destroy.
        if (held.mTextures == nullptr)
            return;

        held.mTextures->drop(textures, mRing.recording().mWorld.mGraveyard);
    }

    bool VulkanRenderer::recordPlacement(
        const SkinPass& skin, ViewScene& held, const SceneDesc& scene, const Placing& placing)
    {
        // What the scene let go of, given back here: walking away from a ring frees its meshes and
        // nothing arrives to take them over until the next ring, so a frame that only places is the
        // one that must not hold their structures.
        held.mAcceleration->release(scene.meshes().getFreed(), placing.mGraveyard);

        // Once, for the slots that changed, and both halves read it: a nine-by-nine exterior is
        // fifty thousand rows with a matrix inverse apiece, and a frame changes a hundred.
        updateInstanceRecords(scene, held.mRecords, held.mChangedRecords);

        // **The pose first, because the refit reads it.** Every skinned body and morphed face this
        // copy owes is computed into it here, and the barrier the pass ends in is what the refit
        // and the trace wait on.
        const bool posed = skin.record(placing.mCommands, scene, placing.mSlot, *held.mSkinTables,
            held.mAcceleration->getPoses(), held.mBuffers->getNormals(), placing.mTimer);

        const bool built = held.mAcceleration->place(scene, held.mRecords, held.mChangedRecords, placing);

        // Nothing to report, because nothing here is recorded: the tables are host-visible and the
        // submit that follows makes them visible. Only what a moving world changed — rebuilding all
        // of it is tens of milliseconds on a nine-by-nine region.
        held.mBuffers->place(scene, held.mRecords, held.mChangedRecords, placing.mSlot, placing.mGraveyard);

        return posed || built;
    }

    void VulkanRenderer::placeScene(const SceneSlot slot, const SceneDesc& scene, const SeaState& sea)
    {
        ViewScene& held = sceneAt(slot);
        assert(held.mAcceleration != nullptr && "placeScene before setScene");

        // The copy this placement writes is the one the last frame did not trace, and whatever
        // frame last traced it is waited for here — usually a comparison, and where the GPU is
        // behind, the right place for the CPU to stand still. The other copy and not a parity of
        // its own, because a frame need not place.
        const FrameSlot into = held.mSlot.next();
        if (held.mReadBy[into.get()] != sNeverRead)
        {
            // **A picture recorded this frame and carried by nothing yet reads this copy too**, and
            // the ring cannot wait for a frame that was never submitted. Two placements of one
            // scene inside one frame is the only way here, which a game never takes.
            if (held.mReadBy[into.get()] >= mRing.getRecording())
                mPool.finishDeferred();

            mRing.finishThrough(held.mReadBy[into.get()]);
        }

        // A picture inside the interface is placed into a batch that rides the next submit, neither
        // timed nor opening the frame's report; the trace that follows is deferred the same way, and
        // the barrier `place` ends in orders the pair.
        if (!slot.isWorld())
        {
            Batch placement(mPool);
            recordPlacement(mSkinPass, held, scene,
                Placing{
                    .mCommands = placement.getCommands(),
                    .mSlot = into,
                    .mGraveyard = mRing.recording().mWorld.mGraveyard,
                });
            placement.defer();
            held.mSlot = into;
            return;
        }

        // **A placement opens the frame, and every placement before a trace joins it.** The frame's
        // report starts here and not at the trace: placing the world is the refit and the top level,
        // and a report that began at `renderFrame` would leave them out.
        FrameRecord& frame = mRing.begin();

        // Does nothing where the sea is the one already drawn for, which is every frame but the
        // first and any on which the weather turned the wind.
        mWaves.describe(sea, frame.mWorld.mGraveyard);

        // **The placement's own submit, without a fence and without a wait.** The frame's fence,
        // later on the queue, covers this submit too. Nothing recorded is nothing submitted, which
        // is every frame of a standing camera in an empty place.
        const VkCommandBuffer placement = mRing.takePlaceCommands(frame);
        mPool.begin(placement);

        if (recordPlacement(mSkinPass, held, scene,
                Placing{
                    .mCommands = placement,
                    .mSlot = into,
                    .mTimer = &frame.mTimer,
                    .mGraveyard = frame.mWorld.mGraveyard,
                }))
            mPool.submit(placement, VK_NULL_HANDLE, frame.mWorld.mGraveyard);
        else
            checkVk(vkEndCommandBuffer(placement), "vkEndCommandBuffer");

        held.mSlot = into;

        readPlacedStats(held);
    }

    MemoryReport VulkanRenderer::getMemoryReport() const
    {
        return mDevice.getMemory().report();
    }

    void VulkanRenderer::readPlacedStats(const ViewScene& held)
    {
        mStats.mInstances = held.mAcceleration->getInstanceCounts();
        mStats.mTableBytes = held.mBuffers->getBytes() + held.mSkinTables->getBytes();

        // **Read every placement and not with the rest of the report**, because a placement is
        // where the answer lands: the queries a build wrote are read some placements later, so a
        // pair read at the build would be the nought that stands between the question and its
        // answer. `BottomLevelStore::getCompactableBytes` says why it is not asked for sooner.
        mStats.mCompactableBytes = held.mAcceleration->getCompactableBytes();
        mStats.mCompactableNowBytes = held.mAcceleration->getCompactableNowBytes();
    }

    void VulkanRenderer::readStats(const ViewScene& held)
    {
        readPlacedStats(held);

        mStats.mStructureBytes = held.mAcceleration->getStructureBytes();
        mStats.mStructureLiveBytes = held.mAcceleration->getStructureLiveBytes();

        const TexturesHeld textures = held.mTextures->getHeld();
        mStats.mTextureCount = textures.mCount;
        mStats.mTextureBytes = textures.mBytes;
    }

    void VulkanRenderer::setVerticalSync(SDLUtil::VSyncMode mode)
    {
        // Headless: `shot`, `bench` and `verify` present to nothing, and a run with no surface has
        // no refresh to meet.
        if (mPresenter == nullptr)
            return;

        // The swapchain goes with the command pool a handed-over batch is sitting in, exactly as a
        // resize does.
        mGuiTextures.finish();
        mPresenter->setVerticalSync(mode);
    }

    std::optional<FrameResult> VulkanRenderer::finishFrame()
    {
        return mRing.collect();
    }

    void VulkanRenderer::resize(std::uint32_t width, std::uint32_t height)
    {
        if (mPresenter != nullptr)
        {
            // **Asked before anything is drained, because `fitToWindow` calls this every settled
            // frame.** Same reason as the destructor's: remaking a swapchain resets the command
            // pool, and a batch handed over is sitting in it waiting for a submit. What that costs
            // where no rebuild follows is `Presenter::wantsResize`.
            if (mPresenter->wantsResize(VkExtent2D{ width, height }))
            {
                mGuiTextures.finish();
                mPresenter->rebuild(VkExtent2D{ width, height });
            }

            // **What the swapchain came back with, not what was asked for.** A surface clamps to
            // what it can do, and targets sized to the request would then be blitted through a
            // scale nobody chose.
            const VkExtent2D shown = mPresenter->getExtent();
            width = shown.width;
            height = shown.height;
        }

        if (width == mOutputWidth && height == mOutputHeight)
            return;

        // The images about to be replaced may still be in flight.
        mRing.finishAll();
        mDevice.waitIdle();
        createTargets(width, height);
    }

    GuiSlot VulkanRenderer::addGuiTexture(std::uint32_t width, std::uint32_t height)
    {
        return mGuiTextures.add(width, height);
    }

    void VulkanRenderer::writeGuiTexture(
        const GuiSlot texture, const GuiRegion& region, std::span<const std::uint8_t> rgba)
    {
        mGuiTextures.write(texture, region, rgba);
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

        // The interface drawn two frames ago drew out of this slot; its fence is what says the
        // vertices may be written over.
        FrameRecord& gui = mRing.slotOf(mGuiFrame);
        if (gui.mGui.mPending)
        {
            awaitVk(mDevice, gui.mGui.mFence.get(), "the interface drawn two frames ago");
            gui.mGui.mPending = false;
            gui.mGui.mGraveyard.clear();
        }

        // After the clear and before anything is handed over: this frame's fence is the first
        // that says every draw with a texture given back has finished, and the staging turns on
        // the same signal.
        mGuiTextures.startFrame(gui.mGui.mGraveyard);

        gui.mGui.mGraveyard.bury(
            growTo(gui.mGuiVertices, mDevice, vertices.size_bytes(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT));
        gui.mGuiVertices.write(vertices);

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

        // **Its own submit, after the frame's, and not waited for.** The GUI is collected once the
        // world has been drawn and there is nothing to gain by holding the frame open for it; the
        // queue draws it after the frame, the present blits after both, and the fence is for the
        // vertices alone.
        mPool.begin(gui.mGui.mCommands);

        const VkCommandBuffer commands = gui.mGui.mCommands;
        mTargets.current().transition(commands, Use::sComputeWrite, Use::sColourAttachment);

        mGuiPass.record(commands, mTargets.current(), gui.mGuiVertices.getHandle(), mGuiDraws);

        // Back where everything else expects it: the presenter blits out of `GENERAL` and so
        // does a read back.
        mTargets.current().transition(commands,
            ImageUse{ VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT },
            Use::sAnyGeneralRead);

        mPool.submit(commands, gui.mGui.mFence.get(), gui.mGui.mGraveyard);
        gui.mGui.mPending = true;
        ++mGuiFrame;
    }

    bool VulkanRenderer::presentFrame()
    {
        assert(mPresenter != nullptr && "presentFrame on a renderer that was given no window");
        assert(mTargets.isOpen());

        const bool shown = mPresenter->present(mTargets.current());

        mTargets.presented([this](const Image& next) { mPresenter->waitForLastUse(next); });

        return shown;
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
        assert(mPass != nullptr && "renderFrame before setScene");
        assert(camera.mCamera.mWidth == mFrame.getWidth() && camera.mCamera.mHeight == mFrame.getHeight()
            && "the camera has to be built for the render extent; ask getExtents");

        // Coverage and an upscaler do not meet: NGX writes the upscaled image itself and was never
        // given `pInAlpha`, so a picture that stops where nothing was hit is `traceGuiTexture`'s.
        assert((camera.mTransparentBackground == 0 || mUpscaling.mMode == Upscale::Off)
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
        if (mCountHits || mCountCrossings)
            *static_cast<FrameCounts*>(frame.mHitCount.map()) = FrameCounts{};

        // **What reconstructs this frame, decided once and by one rule.** Every switch below reads
        // this rather than working the interaction out again; the same value goes back in the frame
        // result, so what a run reports and what it did are one answer.
        const Reconstruction reconstruction = Reconstruction::resolve(mUpscaling, options.mReconstruction);
        frame.mReconstruction = reconstruction;

        const Shaders::VisibilityConstants sampled = sampleCamera(camera, reconstruction);

        const VisibilityInputs inputs = describeInputs(mWorld, &mFrame.getFogVolume(), camera.mRayMask);

        // Made by the first frame that averages, and that frame is the one that fills it.
        const bool fresh = options.mAccumulate > 0 && mSum == nullptr;
        if (fresh)
            mSum = std::make_unique<Image>(mDevice, mFrame.getWidth(), mFrame.getHeight(),
                VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, "sum");

        // A history is worthless after a jump no motion vector can describe: walking through a
        // door once left the previous camera intact and a reprojection fetched one room onto
        // another.
        FrameHistory history(mPreviousCamera.mCamera.mForward.length2() <= 0.0f, mAirStale, mDenoiserStale);

        GpuTimer& timer = frame.mTimer;
        const VkCommandBuffer commands = frame.mWorld.mCommands;
        mPool.begin(commands);

#ifdef OPENMW_RTX_DLSS
        // `createTargets` makes the pass and its image together and releases them together, so
        // nothing below asks whether they are there.
        assert(!upscaling() || (mUpscaler != nullptr && mUpscaled != nullptr));

        if (upscaling())
            mUpscaled->transition(commands,
                ImageUse{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT },
                Use::sAnyGeneralWrite);
#endif

        // The first write needs no contents and nothing to wait on; every one after reads what
        // the last left, which the queue orders and does not make visible.
        if (mSum != nullptr)
            mSum->transition(commands,
                ImageUse{ fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                    fresh ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    fresh ? 0 : VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT },
                Use::sComputeReadWrite);

        // What the bin inside the recording writes may still be being traced: two frames with no
        // placement between them bin into the one copy. On the ordinary path this was waited by
        // `placeScene` and costs a compare.
        if (mWorld.mReadBy[mWorld.mSlot.get()] != sNeverRead)
            mRing.finishThrough(mWorld.mReadBy[mWorld.mSlot.get()]);

        // **Ray Reconstruction is itself the denoiser**, and handing it a frame the wavelet already
        // blurred is asking it to recover what was thrown away — which is why `resolve` never
        // answers with both.
        const bool filtering = reconstruction.filtered();

        const GBuffer& channels = mFrame.getChannels();

        const Image* shown = &mFrame.record(commands,
            TraceRecording{
                .mVisibility = mPass.get(),
                .mComposite = &mComposite,
                .mSpriteBin = &mSpriteBin,
                .mSpriteShade = &mSpriteShade,
                .mInputs = inputs,
                .mBuffers = mWorld.mBuffers.get(),
                .mGraveyard = &frame.mWorld.mGraveyard,
                .mAsked = camera,
                .mSampled = sampled,
                .mCounts = &frame.mHitCount,
                .mTarget = &mTargets.current(),
                .mSum = mSum.get(),
                .mAccumulate = options.mAccumulate,
                .mAirLost = history.airLost(),
                .mHistoryLost = history.answer(filtering),
                .mFilter = filtering,
                .mTimer = &timer,
            });

#ifdef OPENMW_RTX_DLSS
        if (upscaling())
        {
            timer.open(commands, "upscale");
            mUpscaler->record(commands,
                DlssInputs{
                    .mColour = mFrame.getColour(),
                    .mDiffuseAlbedo = channels.get(Channel::Albedo),
                    .mSpecularAlbedo = channels.get(Channel::Specular),
                    .mNormalRoughness = channels.get(Channel::Guide),
                    .mDepth = channels.get(Channel::Depth),
                    .mMotion = channels.get(Channel::Motion),
                    .mReflectionMotion = channels.get(Channel::ReflectionMotion),
                    .mParticleMask = channels.get(Channel::ParticleMask),
                    .mTransparency = channels.get(Channel::Transparency),
                    .mTransparencyOpacity = channels.get(Channel::TransparencyOpacity),
                    .mTransparencyMotion = channels.get(Channel::TransparencyMotion),
                    .mBiasMask = channels.get(Channel::BiasMask),
                    .mOutput = *mUpscaled,
                    .mJitter = sampled.mCamera.mJitter,
                    .mFrameDeltaMs = sinceLastMs,
                    .mReset = history.answer(),
                });

            // What NGX recorded is its own; nothing here knows which stages it used. **And the
            // bloom samples what it left**, rather than loading it — `BloomPass` binds the frame as
            // a combined image sampler — so a visibility scope of storage reads alone would leave
            // that read uncovered.
            mUpscaled->transition(commands, Use::sAnyGeneralWrite, Use::sComputeReadOrSample);

            timer.close(commands);
            shown = mUpscaled.get();
        }
#endif

        // **What the lens will spread, built here and applied by the curve.** Nothing is
        // written back over the frame — `BloomPass` says why the trace's own answer has to
        // reach `FrameImage::Composite` untouched.
        timer.open(commands, "bloom");
        mBloom.record(commands, *shown);
        timer.close(commands);

        // **Measured off the image the curve is about to map**, which is the upscaled one
        // wherever something upscales — see `histogram.comp` for what measuring the other one
        // costs. One `shown` feeds both, so the two cannot come apart.
        timer.open(commands, "exposure");
        if (options.mExposure.has_value())
            mExposure.recordFixed(commands, *options.mExposure);
        else
        {
            // **The third thing that reads a lost history**, and the only one that reads it on
            // every frame: the eye has no past to adapt from either.
            mExposure.record(commands, *shown, 0.001f * sinceLastMs, history.answer(), options.mExposureBias);
        }
        timer.close(commands);

        timer.open(commands, "tone");
        mTone->record(commands, *shown, mExposure.getExposure(), channels.get(Channel::StarsShown), mBloom.getPyramid(),
            inputs.mTextures, mTargets.current(),
            toneFor(sampled, mOutputWidth, mOutputHeight, channels.getWidth(), channels.getHeight()));
        timer.close(commands);

        // Submitted and not waited for: `finishFrame` brings the count and the report back a
        // frame late. A fence's access scope is the device's, so the counters need a dependency
        // of their own, recorded here after every pass that could have added to them.
        if (mCountHits || mCountCrossings)
            frame.mHitCount.orderForHostRead(commands);

        mWorld.mReadBy[mWorld.mSlot.get()] = mRing.getRecording();
        mRing.submit(frame);

        // What the next frame reprojects against, and the camera as the caller gave it: a jitter is
        // where inside a pixel this frame sampled, not where the eye was.
        mPreviousCamera = camera;

        // **The air's is spent here and unconditionally, because the trace above always ran.** A
        // frame that filled the volume was told; a frame with no volume to fill has nothing to keep
        // a stale flag for, and holding it would zero the basis — and so every motion vector — for
        // as long as the player stayed indoors.
        mAirStale = false;

        if (history.wasAnswered())
            mDenoiserStale = false;

        return reconstruction;
    }

    SceneSlot VulkanRenderer::addViewScene()
    {
        if (const Index taken = mFreeViewScenes.take(); taken != sNoIndex)
        {
            mViewScenes[taken] = std::make_unique<ViewScene>();
            return SceneSlot::view(taken);
        }

        mViewScenes.push_back(std::make_unique<ViewScene>());
        return SceneSlot::view(static_cast<std::uint32_t>(mViewScenes.size() - 1));
    }

    void VulkanRenderer::dropViewScene(const SceneSlot scene)
    {
        assert(scene.getViewIndex() < mViewScenes.size() && mViewScenes[scene.getViewIndex()] != nullptr
            && "a scene given back twice");

        // **What a picture's placement buried is this scene's**, and the frame it was buried under
        // need never be traced — so it is given back here rather than to a scene that has gone. A
        // picture of it recorded this frame and not yet carried goes first, or it would be carried
        // over a scene that no longer exists.
        mPool.finishDeferred();
        mDevice.waitIdle();
        mRing.finishAll();
        mRing.emptyGraveyards();

        mViewScenes[scene.getViewIndex()].reset();
        mFreeViewScenes.free(scene.getViewIndex());
    }

    void VulkanRenderer::growViewTargets(std::uint32_t width, std::uint32_t height)
    {
        if (mView.holds(width, height))
            return;

        // **The one drain a picture still pays, and only the first picture of a new size pays it.**
        // A picture recorded and not yet carried, or carried and not yet finished, names the images
        // about to be replaced.
        mPool.finishDeferred();
        mRing.finishAll();

        mView.grow(width, height, false);

        mViewTarget = std::make_unique<Image>(mDevice, mView.getWidth(), mView.getHeight(), PresentTargets::sFormat,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "view target");
    }

    void VulkanRenderer::traceGuiTexture(
        const GuiSlot texture, const Shaders::VisibilityConstants& camera, const GuiTraceOptions& options)
    {
        assert(mPass != nullptr && "traceGuiTexture before any scene was built");
        assert(camera.mCamera.mWidth == options.mWidth && camera.mCamera.mHeight == options.mHeight
            && "the camera has to be built for the part of the texture it fills");

        const bool held = mGuiTextures.holds(texture);
        assert(held && "a trace into a slot nothing holds");

        if (!held || options.mWidth == 0 || options.mHeight == 0)
            return;

        growViewTargets(options.mWidth, options.mHeight);

        // A picture composites its own transparency, for the reason `mLayerCompositedAfter` gives:
        // nothing upscales one, so nothing would read the layer it handed over.
        assert(camera.mLayerCompositedAfter == 0 && "a picture inside the interface hands its layer to nobody");

        ViewScene& traced = sceneAt(options.mScene);

        const VisibilityInputs inputs = describeInputs(traced, &mView.getFogVolume(), camera.mRayMask);

        // The scene's own, filled here rather than by the caller, because whether the scene behind
        // a camera holds a cloud is this renderer's to answer. The only one of `sampleCamera`'s
        // fields a picture wants. Copied because the caller's block is theirs.
        Shaders::VisibilityConstants sampled = camera;
        sampled.mMediumInFrame = traced.mAcceleration->getInstanceCounts().mMedium > 0 ? 1 : 0;

        // Recorded into a batch that rides the next submit, and waited for by nobody here: the
        // next placement of this scene waits through `mReadBy`, and what it writes nothing else
        // reads. Not counted and not timed, because the hit count and the report are the frame's.
        Batch trace(mPool);
        {
            const VkCommandBuffer commands = trace.getCommands();
            const GBuffer& channels = mView.getChannels();

            // A doll and a map tile are one frame with no frame before them, so every history says
            // so: the accumulator becomes a pass-through handing on the largest variance there is,
            // which is what tells the cascade to filter as widely as it can.
            mView.record(commands,
                TraceRecording{
                    .mVisibility = mPass.get(),
                    .mComposite = &mComposite,
                    .mSpriteBin = &mSpriteBin,
                    .mSpriteShade = &mSpriteShade,
                    .mInputs = inputs,
                    .mBuffers = traced.mBuffers.get(),
                    .mGraveyard = &mRing.recording().mWorld.mGraveyard,
                    .mAsked = camera,
                    .mSampled = sampled,
                    .mCounts = &mViewCounts,
                    .mTarget = mViewTarget.get(),
                });

            // One, and measured off nothing, or the same armour would be a different brightness
            // in two windows; out of its own buffer, for what `ExposurePass::getPictureExposure`
            // says. And no lens, because a map tile is a diagram.
            mTone->record(commands, mView.getColour(), mExposure.getPictureExposure(),
                channels.get(Channel::StarsShown), nullptr, inputs.mTextures, *mViewTarget,
                toneFor(camera, options.mWidth, options.mHeight, channels.getWidth(), channels.getHeight()));

            mViewTarget->transition(commands, Use::sComputeWrite, Use::sCopyRead);

            // **Borrowed rather than transitioned.** Where a GUI texture rests between writes is
            // `GuiTextures`' to say, and a caller that said it here had to keep a barrier's scope in
            // step with the commands below — which it did not.
            mGuiTextures.writeWith(texture, commands, [&](const Image& into, VkImageLayout layout) {
                assert(options.mWidth <= into.getWidth() && options.mHeight <= into.getHeight());

                // **Cleared whole and then covered in part**, and only where the picture does not
                // cover it all: what the trace fills is as much of the texture as the widget is
                // currently wide, and the rest has to be the clear colour rather than what a wider
                // picture left there the last time this was drawn.
                if (options.mWidth < into.getWidth() || options.mHeight < into.getHeight())
                {
                    const VkClearColorValue clear{ .float32
                        = { options.mClear[0], options.mClear[1], options.mClear[2], options.mClear[3] } };
                    const VkImageSubresourceRange whole{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                    vkCmdClearColorImage(commands, into.getHandle(), layout, &clear, 1, &whole);

                    // Both are transfer writes to the same image and nothing orders two of those.
                    into.transition(commands,
                        ImageUse{ layout, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT },
                        ImageUse{ layout, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT });
                }

                const VkImageCopy region{
                    .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                    .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                    .extent = { options.mWidth, options.mHeight, 1 },
                };
                vkCmdCopyImage(commands, mViewTarget->getHandle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    into.getHandle(), layout, 1, &region);
            });

            if (options.mReadBack)
                mGuiTextures.readBackWith(texture, commands, mRing.getRecording(), mRing.recording().mWorld.mGraveyard);
        }
        trace.defer();

        // **Conservative where it is not exact.** The batch rides the next submit this pool makes,
        // which is this frame's or an earlier one's GUI; a later frame's fence covers either by
        // queue order.
        traced.mReadBy[traced.mSlot.get()] = mRing.getRecording();
    }

    bool VulkanRenderer::takeGuiCopy(const GuiSlot texture, const std::span<std::uint8_t> into)
    {
        return mGuiTextures.takeCopy(texture, into, mRing.getFinished());
    }

    void VulkanRenderer::finishGuiTraces()
    {
        // Both, because a picture is in one of two places: recorded and carried by nothing, or
        // carried by a frame still in flight.
        mPool.finishDeferred();
        mRing.finishAll();
        mGuiTextures.landTraces();
    }

    void VulkanRenderer::readGuiTexture(const GuiSlot texture, std::vector<std::uint8_t>& pixels)
    {
        mGuiTextures.read(texture, pixels);
    }

    void VulkanRenderer::readPixels(std::vector<std::uint8_t>& pixels)
    {
        assert(mTargets.isOpen());

        // **The frame that was finished, not the one the next will be written into.** A present has
        // already swapped those two; with no window nothing presents, nothing swaps, and the frame
        // just written is still the one `mTarget` names.
        const Image& frame = mTargets.lastPresented() != nullptr ? *mTargets.lastPresented() : mTargets.current();
        frame.read(mPool, VK_IMAGE_LAYOUT_GENERAL, pixels);
    }

    void VulkanRenderer::readChannel(const Channel channel, std::vector<float>& values)
    {
        assert(mFrame.isBuilt());
        assert(mFrame.getChannels().carries(channel) && "a channel this frame stands in for, read back as its own");

        // **One lookup and not a switch of fourteen arms.** A channel is its binding, and the buffer
        // is indexed by it.
        readImage(mFrame.getChannels().get(channel), values);
    }

    void VulkanRenderer::readFrameImage(const FrameImage image, std::vector<float>& values)
    {
        assert(mFrame.isBuilt());

        switch (image)
        {
            case FrameImage::Composite:
                // The frame every channel was gathered to make.
                readImage(mFrame.getColour(), values);
                return;

            case FrameImage::Accumulated:
                // The denoiser's own, so a frame nothing denoised has no answer here — `getBlended`
                // asserts on one rather than handing back whatever the allocation held, and
                // `hasFrameImage` is where a caller asks before it comes to that.
                readImage(mFrame.getBlended(), values);
                return;
        }
    }

    void VulkanRenderer::readImage(const Image& image, std::vector<float>& values)
    {
        std::vector<std::uint8_t> bytes;
        image.read(mPool, VK_IMAGE_LAYOUT_GENERAL, bytes);

        // Every format the renderer reads back is named, and one that is not is a throw rather
        // than a `memcpy`, which is how the motion channels came back as pairs of halves the day
        // they narrowed. Tested on the format, because several macros across three headers name
        // each of these.
        switch (image.getFormat())
        {
            // A mask holds a yes or a no in a byte, so what comes back is widened rather than
            // reinterpreted.
            case GBUFFER_MASK:
                values.resize(bytes.size());
                for (std::size_t at = 0; at < bytes.size(); ++at)
                    values[at] = static_cast<float>(bytes[at]) / 255.0f;

                return;

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
