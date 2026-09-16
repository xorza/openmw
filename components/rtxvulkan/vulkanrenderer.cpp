#include "vulkanrenderer.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <ratio>
#include <span>
#include <string>
#include <utility>

#include <osg/Vec2f>
#include <vulkan/vulkan_core.h>

#include <components/rtx/camera.hpp>
#include <components/rtx/debuglines.hpp>
#include <components/rtx/error.hpp>
#include <components/rtx/frameimage.hpp>
#include <components/rtx/frameworld.hpp>
#include <components/rtx/memoryreport.hpp>
#include <components/rtx/reconstruction.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/camera.h>
#include <components/rtx/shaders/gbuffer.h>
#include <components/rtx/shaders/line.h>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/shaders/tone.h>
#include <components/rtx/shaders/visibility.h>
#include <components/rtx/slot.hpp>
#include <components/rtx/texturedata.hpp>
#include <components/rtx/upscale.hpp>
#include <components/rtx/wavespectrum.hpp>
#include <components/sdlutil/vsyncmode.hpp>

#include "gbuffer.hpp"
#include "graphicspipeline.hpp"
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
#include "timeline.hpp"
#include "tracerecording.hpp"
#include "validation.hpp"
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

        /// The display pass's own description of the frame, on the picture's grid. The alpha
        /// carries the puffs' transmittance wherever it is not the picture's coverage, which is
        /// the same test `spritecomposite.rgen` makes.
        Shaders::ToneConstants toneFor(const Shaders::VisibilityConstants& frame, std::uint32_t width,
            std::uint32_t height, std::uint32_t tracedWidth, std::uint32_t tracedHeight)
        {
            return Shaders::ToneConstants{
                .mTracedWidth = tracedWidth,
                .mTracedHeight = tracedHeight,
                .mCoverAlpha = frame.mTransparentBackground == 0 ? 1u : 0u,
                .mCamera = Shaders::cameraOnGrid(frame.mCamera, width, height),
                .mStars = frame.mStars,
                .mGlareColour = frame.mGlareColour,
                .mGlareAmount = sunGlareAmount(frame),
            };
        }
    }

    VulkanRenderer::VulkanRenderer(const RendererOptions& options)
        : mInstance(options.mValidation, surfaceExtensionsFor(options))
        , mDevice(mInstance, PhysicalDevice::select(mInstance.getHandle()),
              PipelineCacheSpec{ .mDirectory = options.mCacheDirectory, .mShaderDirectory = options.mShaderDirectory },
              deviceExtensionsFor(options))
        , mPool(mDevice)
        , mGraveyard(mDevice, mPool)
        , mShaderDirectory(options.mShaderDirectory)
        , mCountHits(options.mCountHits)
        , mProfile(options.mProfile)
        , mUpscaling(mProfile.mUpscaling)
        , mChannelLayout(GBuffer::describeLayout(mDevice))
        , mFogVolumeLayout(FogVolume::describeLayout(mDevice))
        // `SAMPLED` because an upscaler samples what it is handed, and one bit short of that is a
        // black frame nothing reports. See `GBuffer`, which carries it for the same reason.
        // `TRANSFER_SRC` because `readComposite` copies this out: it is the frame a measurement is
        // taken on, where `readPixels` gives the one a display would show.
        , mFrame(mDevice, mGraveyard, mPool, mChannelLayout, mFogVolumeLayout, options.mShaderDirectory,
              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "colour")
        , mView(mDevice, mGraveyard, mPool, mChannelLayout, mFogVolumeLayout, options.mShaderDirectory,
              VK_IMAGE_USAGE_STORAGE_BIT, "view colour")
        , mComposite(mDevice, mPool, options.mShaderDirectory)
        , mBloom(mDevice, options.mShaderDirectory)
        , mWaves(mDevice, mPool, options.mShaderDirectory)
        , mRipples(mDevice, mPool, options.mShaderDirectory)
        , mFog(mDevice, mPool)
        , mExposure(mDevice, options.mShaderDirectory)
        , mSunGlare(mDevice, options.mShaderDirectory)
        , mSkinPass(mDevice, options.mShaderDirectory)
        , mSpriteBin(mDevice, options.mShaderDirectory)
        , mSpriteShade(mDevice, options.mShaderDirectory)
        , mNoSprites(Buffer::hostWritten(
              mDevice, 2 * sizeof(std::uint32_t), VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, "no sprites"))
        , mViewCounts(
              Buffer::deviceLocal(mDevice, sizeof(FrameCounts), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "picture counts"))
        , mGuiPass(mDevice, options.mShaderDirectory, PresentTargets::sFormat)
        , mLines(mDevice, options.mShaderDirectory, PresentTargets::sFormat)
        , mGuiTextures(mDevice, mGraveyard, mPool)
    {
        // `SPRITE_LIST_UNBINNED` and a count of nought are both nought.
        mNoSprites.clear();

        // Before the first targets, because what to trace at is its answer and not ours.
        if (mUpscaling.mMode != Upscale::Off)
            startUpscaler();

        if (mProfile.mStressOverlapMs > 0.0)
            mStress = std::make_unique<StressPass>(mDevice, mPool, options.mShaderDirectory, mProfile.mStressOverlapMs);

        // Before the first targets, because a windowed renderer is sized by its surface rather
        // than by what the caller guessed the window would come up at.
        if (options.mWindow != nullptr)
            mPresenter = std::make_unique<Presenter>(
                mDevice, mPool, mGraveyard, mInstance.getHandle(), options.mWindow, options.mVerticalSync);

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

        // Before the scenes below it, the dying ones included, which own the storage the buried
        // rooms are rooms in.
        mGraveyard.clear();
        mDyingScenes.clear();
    }

    void VulkanRenderer::startUpscaler()
    {
#ifdef OPENMW_RTX_DLSS
        if (mNgx != nullptr)
            return;

        // A quarter of a second, which is why it waits to be wanted. Bringing the runtime up
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
        // Named rather than quietly ignored. A build that cannot upscale and renders at the
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

    void VulkanRenderer::drain()
    {
        mPool.finishDeferred();
        mRing.finishAll();
        mDevice.waitIdle();

        // The graveyard before the scenes, for the reason the destructor gives: a buried structure
        // gives its room back to the storage its scene owns.
        mGraveyard.clear();
        mDyingScenes.clear();
    }

    void VulkanRenderer::buryDyingScenes()
    {
        // The graveyard first, and against the same value: a structure a dying scene retired is
        // buried with a stamp no later than the scene's own, and gives its room back to a storage
        // the scene owns when it goes.
        mGraveyard.collect();

        const std::uint64_t finished = mDevice.getTimeline().getKnownFinished();
        std::erase_if(mDyingScenes, [finished](const DyingScene& dying) { return dying.mUntil <= finished; });
    }

    void VulkanRenderer::finishTraces()
    {
        mPool.finishDeferred();
        mRing.finishAll();
    }

    void VulkanRenderer::setUpscale(Upscale upscale)
    {
        if (upscale == mUpscaling.mMode)
            return;

        // Before anything is torn down, so a mode this machine cannot reach leaves the renderer
        // drawing exactly as it was rather than half way between two of them.
        if (upscale != Upscale::Off)
            startUpscaler();

        mUpscaling.mMode = upscale;

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
        VkExtent2D render{ width, height };
#ifdef OPENMW_RTX_DLSS
        if (upscaling())
            render = mNgx->getRenderSize(VkExtent2D{ width, height }, mUpscaling.mMode);
#endif
        mFrame.resize(render.width, render.height, mProfile.mRadianceWidth);

        // Two, and interchangeable, because the frame after this one must not rewrite the image
        // the present is still blitting out of. `PresentTargets` is what holds that rule.
        mTargets.resize(mDevice, mPool, mOutputWidth, mOutputHeight);

#ifdef OPENMW_RTX_DLSS
        // Released before the next is built: the feature holds the network's weights for one pair
        // of resolutions, and the image it writes is sixteen bytes a pixel of the output, so neither
        // is left behind for a mode that may not come back.
        mUpscaler.reset();
        mUpscaled = Image();

        if (upscaling())
        {
            // Half floats whatever width the run gave the trace's own composite: this one is shown
            // and never summed. The peak linear radiance a frame of this game reaches is under nine,
            // measured over the view suite and a camera pointed at the noon sun, so a half carries
            // it with four orders of magnitude to spare at a step finer than the display's — which
            // is also `RadianceWidth::Shown`'s argument.
            mUpscaled = Image(mDevice, mOutputWidth, mOutputHeight, VK_FORMAT_R16G16B16A16_SFLOAT,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, "upscaled");

            // Building uploads the network's weights, which is once per resolution rather than
            // once per frame.
            mPool.submitAndWait([&](VkCommandBuffer commands) {
                mUpscaler = std::make_unique<DlssPass>(*mNgx, commands, render,
                    VkExtent2D{ mOutputWidth, mOutputHeight }, mUpscaling.mMode, mUpscaling.mPreset);
            });
        }
#endif

        // Over whatever the frame is by the time the curve maps it, which is the upscaler's
        // output where one runs and the trace's own extent where none does. The same test the frame
        // path makes, because a pyramid built at the other extent is a bloom at the wrong scale.
        const std::uint32_t shownWidth = upscaling() ? mOutputWidth : mFrame.getWidth();
        const std::uint32_t shownHeight = upscaling() ? mOutputHeight : mFrame.getHeight();
        mBloom.resize(shownWidth, shownHeight);

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

#ifdef OPENMW_RTX_DLSS
        report += "\nDLSS Ray Reconstruction: ";
        try
        {
            // An answer rather than a runtime, which is why reporting on a device cannot disturb
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
            .mTextures = held.mTextures->getSet(held.mSlot),
            .mWaves = &mWaves,
            .mRipples = &mRipples,
            .mFog = &mFog,
            .mFogVolume = volume,
            .mSpriteList = (rayMask & Shaders::MASK_PARTICLE) != 0 ? 0 : mNoSprites.addressFor(),
            .mSunGlare = &mSunGlare.getCounts(),
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

        // The arms' eye samples where the world's does, or the two halves of one frame would be
        // reconstructed from two grids.
        sampled.mArms.mJitter = sampled.mCamera.mJitter;
        sampled.mArmsSpread = armsSpreadOf(camera);

        // The scene's answer and not the camera's, for the reason `VisibilityInputs::mWater` is one:
        // a cell with no cloud in it has nothing for the medium walk to find, wherever it is looked
        // at from. The arms are the scene's and the camera's both: a map draws none.
        const InstanceCounts& counts = mWorld.mAcceleration->getInstanceCounts();
        sampled.mMediumInFrame = counts.mMedium > 0 ? 1 : 0;
        sampled.mAdditiveInFrame = counts.mAdditive > 0 ? 1 : 0;
        sampled.mArmsInFrame = counts.mFirstPerson > 0 && (camera.mRayMask & Shaders::MASK_FIRST_PERSON) != 0 ? 1 : 0;

        // The one subtraction of two world points, and it happens here. Two camera positions a
        // step apart subtract exactly in a float; the same difference taken on the device, between
        // coordinates six figures long, would be rounding.
        sampled.mCameraMotion = camera.mOrigin - mPreviousCamera.mOrigin;
        sampled.mPreviousForward = mPreviousCamera.mCamera.mForward;
        sampled.mPreviousRight = mPreviousCamera.mCamera.mRight;
        sampled.mPreviousUp = mPreviousCamera.mCamera.mUp;

        return sampled;
    }

    void VulkanRenderer::setScene(const SceneSlot slot, const SceneDesc& scene, std::span<const TextureData> textures)
    {
        ViewScene& held = sceneAt(slot);

        // Nothing may be in flight over what is about to go. A rebuild is a load, and a load
        // waits: for a picture recorded against the old scene and not yet carried, for the frames
        // tracing it, and for a placement the frame being recorded may have submitted without a
        // fence of its own.
        drain();

        // Torn down before anything is built, so a second scene does not hold two of everything at
        // once — a cell's structures and textures are most of what this renderer occupies. The pass
        // is not among them; see below.
        held.mTextures.reset();
        held.mSkinTables.reset();
        held.mBuffers.reset();
        held.mAcceleration.reset();

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

        // The copies are new and alike, so nothing has read either.
        held.mSlot = FrameSlot{};
        held.mPictureRides.fill(0);

        // Made here for the same reason a frame's are: both of the two below want them, and this is
        // the only place that knows both.
        makeInstanceRecords(scene, held.mRecords);

        // One submit for the whole cell. Every structure, every table and every texture is
        // recorded into this and the queue is asked once, at the flush below; a round trip apiece
        // would be hundreds for a town.
        Batch setup(mPool);

        // Every scene is traced by two frames at once, the doll's included: a picture inside the
        // interface rides the frame it was asked on, and the next frame may place it again while
        // that one is still tracing.
        held.mAcceleration = std::make_unique<SceneAcceleration>(mDevice, mGraveyard, setup, scene, sFrameSlots);
        held.mBuffers = std::make_unique<SceneBuffers>(mDevice, mGraveyard, setup, scene, held.mRecords, sFrameSlots);
        held.mSkinTables = std::make_unique<SkinTables>(mDevice, mGraveyard, setup, scene, sFrameSlots);

        held.mTextures = std::make_unique<TextureArray>(
            mDevice, mGraveyard, setup, static_cast<std::uint32_t>(scene.textures().getPaths().size()), textures);

        // Built once and kept, because building one compiles every kernel the trace can ever need
        // — 6.3 s on a cold cache, measured. Every texture array declares the same bindless
        // layout, and identically defined layouts are compatible, so the pass belongs to neither
        // scene.
        if (mPass == nullptr)
        {
            mPass = std::make_unique<VisibilityPass>(mDevice, setup, mShaderDirectory, held.mTextures->getLayout(),
                mChannelLayout, mFogVolumeLayout, mCountHits);
            mTone = std::make_unique<TonePass>(mDevice, mPool, held.mTextures->getLayout(), mShaderDirectory);
        }

        // Posed before it is built. The structures are built over the first copy of the
        // positions, and a skinned body's bind pose is not where the body is; the pass writes the
        // pose into that copy and the build then reads it. The other copy is owed the same pose and
        // takes it on the first placement that writes it.
        mSkinPass.record(setup.getCommands(), scene, FrameSlot{}, *held.mSkinTables, held.mAcceleration->getPoses(),
            held.mBuffers->getNormals(), nullptr);
        held.mAcceleration->build(setup, scene, held.mRecords);
        held.mBuiltMeshes = scene.meshes().getRevision();
        held.mBuiltStructure = scene.getStructureRevision();

        // The first copy's set, which the first frame binds before any placement pays it.
        held.mTextures->sync(FrameSlot{});

        // By hand rather than left to the destructor, so a submit that fails throws out of here
        // instead of being logged on the way past.
        setup.flush();

        if (slot.isWorld())
            readStats(held);
    }

    void VulkanRenderer::extendScene(const SceneSlot slot, const SceneDesc& scene, std::span<const TextureData> arrived)
    {
        ViewScene& held = sceneAt(slot);
        assert(held.mAcceleration != nullptr && "extendScene before setScene");

        // An arrival does not wait for the frames in flight: what arrives is written on the queue,
        // behind whatever a frame in flight still reads of the room it was given, and the writes
        // end in the barrier `orderStagedWrites` records. It opens the frame it lands in, because
        // `beginFrame` clears the timer and a zone opened before it would be forgotten.
        GpuTimer* timer = nullptr;
        if (slot.isWorld())
            timer = &mRing.begin().mTimer;

        Batch setup(mPool);
        held.mTextures->write(setup, arrived);

        // The meshes that arrived, and no others: the geometry blocks are appended to rather than
        // replaced, so every address a structure was built from is still its own. The revision and
        // not the count, because a freed slot taken over holds different geometry at the same size.
        if (scene.meshes().getRevision() != held.mBuiltMeshes)
        {
            held.mBuffers->extend(setup, scene);
            held.mSkinTables->extend(setup, scene);
            held.mAcceleration->extend(setup, scene);

            // Posed before it is built, as `setScene` does, into the first copy, which is what the
            // build reads — and only the meshes that arrived, over the rows `SkinTables::extend`
            // staged. `SkinPass::recordArrived` says why it may not be every mesh the copy owes.
            mSkinPass.recordArrived(setup.getCommands(), scene, FrameSlot{}, scene.meshes().getArrived(),
                *held.mSkinTables, held.mAcceleration->getPoses(), held.mBuffers->getNormals());
            held.mAcceleration->buildArrived(setup, scene, timer);
            held.mBuiltMeshes = scene.meshes().getRevision();
        }

        // Deferred to the placement's submit: `placeScene` submits this ahead of the refit and the
        // top level, and the barrier every upload and build ends in orders them, so a composite
        // landing costs no submit, fence or wait of its own.
        setup.defer();

        held.mBuiltStructure = scene.getStructureRevision();

        // Always, because the top level names every instance and an arrival changed the list. It is
        // rebuilt every frame regardless, so an arrival costs it nothing.
        placeScene(slot, scene);

        // The history is kept. Nothing was renumbered, so what the last frame resolved still
        // describes the same surfaces — and throwing it away is a visible flash every time an actor
        // walks into view with a texture nobody has worn yet.
        if (slot.isWorld())
            readStats(held);
    }

    void VulkanRenderer::ViewScene::finishReads(const FrameSlot slot) const
    {
        mBuffers->finishReads(slot);
        mAcceleration->finishReads(slot);
        mSkinTables->finishReads(slot);
        mTextures->finishReads(slot);
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

        held.mTextures->drop(textures);
    }

    bool VulkanRenderer::recordPlacement(
        const SkinPass& skin, ViewScene& held, const SceneDesc& scene, const Placing& placing)
    {
        // What the scene let go of, given back here: walking away from a ring frees its meshes and
        // nothing arrives to take them over until the next ring, so a frame that only places is the
        // one that must not hold their structures.
        held.mAcceleration->release(scene.meshes().getFreed());

        // Once, for the slots that changed, and both halves read it: a nine-by-nine exterior is
        // fifty thousand rows with a matrix inverse apiece, and a frame changes a hundred.
        updateInstanceRecords(scene, held.mRecords, held.mChangedRecords);

        // The descriptors this copy's set owes, now that nothing on the queue reads it.
        held.mTextures->sync(placing.mSlot);

        // The pose first, because the refit reads it. Every skinned body and morphed face this
        // copy owes is computed into it here, and the barrier the pass ends in is what the refit
        // and the trace wait on.
        const bool posed = skin.record(placing.mCommands, scene, placing.mSlot, *held.mSkinTables,
            held.mAcceleration->getPoses(), held.mBuffers->getNormals(), placing.mTimer);

        const bool built = held.mAcceleration->place(scene, held.mRecords, held.mChangedRecords, placing);

        // Nothing to report, because nothing here is recorded: the tables are host-visible and the
        // submit that follows makes them visible. Only what a moving world changed — rebuilding all
        // of it is tens of milliseconds on a nine-by-nine region.
        held.mBuffers->place(scene, held.mRecords, held.mChangedRecords, placing);

        return posed || built;
    }

    void VulkanRenderer::placeScene(const SceneSlot slot, const SceneDesc& scene)
    {
        ViewScene& held = sceneAt(slot);
        assert(held.mAcceleration != nullptr && "placeScene before setScene");

        // The copy this placement writes is the one the last frame did not trace. The other copy
        // and not a parity of its own, because a frame need not place.
        const FrameSlot into = held.mSlot.next();

        // A picture of this copy recorded and carried by nothing yet is carried first, for what
        // `ViewScene::mPictureRides` says. Three placements of one scene inside one frame is the
        // only way here, which a game never takes.
        if (held.mPictureRides[into.get()] == mDevice.getTimeline().getNext())
            mPool.finishDeferred();

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
            Batch placement(mPool);
            recordPlacement(mSkinPass, held, scene,
                Placing{
                    .mCommands = placement.getCommands(),
                    .mSlot = into,
                });
            placement.defer();
            held.mSlot = into;
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
        mPool.begin(placement);

        if (recordPlacement(mSkinPass, held, scene,
                Placing{
                    .mCommands = placement,
                    .mSlot = into,
                    .mTimer = &frame.mTimer,
                }))
            mPool.submit(placement, mGraveyard);
        else
            mPool.end(placement);

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

        // Read every placement and not with the rest of the report, because a placement is
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
        // Headless: `shot`, `bench` and `check` present to nothing, and a run with no surface has
        // no refresh to meet.
        if (mPresenter == nullptr)
            return;

        // A handed-over batch is submitted first, exactly as a resize does.
        mGuiTextures.finish();
        mPresenter->setVerticalSync(mode);
    }

    std::optional<FrameResult> VulkanRenderer::finishFrame()
    {
        std::optional<FrameResult> result = mRing.collect();
        buryDyingScenes();
        return result;
    }

    std::optional<FrameResult> VulkanRenderer::collectFrame()
    {
        std::optional<FrameResult> result = mRing.collectFinished();
        buryDyingScenes();
        return result;
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
        {
            gui.mGuiVertices.waitIdle("the interface drawn two frames ago");
            mGraveyard.collect();
        }

        // After the collect and before anything is handed over: this frame's submit is the first
        // that says every draw with a texture given back has finished, and the staging turns on
        // the same signal.
        mGuiTextures.startFrame();

        mGraveyard.bury(growTo(gui.mGuiVertices, mDevice, BufferKind::HostWritten, vertices.size_bytes(),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "gui vertices"));
        gui.mGuiVertices.write(vertices);

        // Named by hand, because a vertex buffer is bound by handle and not handed out as an
        // address or a descriptor.
        gui.mGuiVertices.nameFor(mDevice.getTimeline().getNext());

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
        mPool.begin(commands);
        claimTarget().transition(commands, Use::sComputeWrite, Use::sColourAttachment);

        mGuiPass.record(commands, mTargets.current(), gui.mGuiVertices.getHandle(), mGuiDraws);

        // Back where everything else expects it: the presenter blits out of `GENERAL` and so
        // does a read back.
        mTargets.current().transition(commands,
            ImageUse{ VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT },
            Use::sAnyGeneralRead);

        mPool.submit(commands, mGraveyard);
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

    void VulkanRenderer::recordDebugLines(const VkCommandBuffer commands, FrameRecord& frame,
        const Shaders::VisibilityConstants& sampled, const GBuffer& channels, const DebugLines& debug, GpuTimer& timer)
    {
        if (debug.empty())
            return;

        // The lines first and the triangles after them, in the slot's own buffer: the frame
        // behind read its own slot's, so nothing here is written under a submit.
        const std::size_t count = debug.mLines.size() + debug.mTriangles.size();
        mGraveyard.bury(growTo(frame.mDebugVertices, mDevice, BufferKind::HostWritten, count * sizeof(DebugVertex),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "debug vertices"));

        const std::span<DebugVertex> written = frame.mDebugVertices.writable<DebugVertex>(0, count);
        std::copy(debug.mLines.begin(), debug.mLines.end(), written.begin());
        std::copy(debug.mTriangles.begin(), debug.mTriangles.end(), written.begin() + debug.mLines.size());

        timer.open(commands, "lines");

        // Drawn over what the curve wrote, and left where the curve left it: the interface and
        // the presenter both take the target from there.
        Image& target = mTargets.current();
        target.transition(commands, Use::sComputeWrite, Use::sColourAttachment);

        mLines.record(commands, target, channels.get(Channel::Depth),
            Shaders::LineConstants{
                .mCamera = Shaders::cameraOnGrid(sampled.mCamera, target.getWidth(), target.getHeight()),
                .mOrigin = sampled.mOrigin,
                .mNear = sampled.mNear,
                .mTraced = Shaders::uvec2(channels.getWidth(), channels.getHeight()),
            },
            frame.mDebugVertices.getHandle(), static_cast<std::uint32_t>(debug.mLines.size()),
            static_cast<std::uint32_t>(debug.mTriangles.size()));

        target.transition(commands, Use::sColourAttachment, Use::sComputeWrite);

        timer.close(commands);
    }

    Image& VulkanRenderer::claimTarget()
    {
        return mTargets.claim([this](const Image& target) {
            if (mPresenter != nullptr)
                mPresenter->waitForLastUse(target);
        });
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
        if (mCountHits)
            frame.mHitCount.writable<FrameCounts>(0, 1).front() = FrameCounts{};

        // What reconstructs this frame, decided once and by one rule. Every switch below reads
        // this rather than working the interaction out again; the same value goes back in the frame
        // result, so what a run reports and what it did are one answer.
        const Reconstruction reconstruction = Reconstruction::resolve(mUpscaling, options.mReconstruction);
        frame.mReconstruction = reconstruction;

        Shaders::VisibilityConstants sampled = sampleCamera(camera, reconstruction);

        VisibilityInputs inputs = describeInputs(mWorld, &mFrame.getFogVolume(), camera.mRayMask);

        // Where the puffs are composited: over the reconstruction where something upscales, and
        // over the trace's own composite where nothing does. Named before the trace, because the
        // set that carries it is pushed for every launch.
        inputs.mShown = upscaling() ? &mUpscaled : &mFrame.getColour();

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
        mPool.begin(commands);

#ifdef OPENMW_RTX_DLSS
        // `createTargets` makes the pass and its image together and releases them together, so
        // nothing below asks whether they are there.
        assert(!upscaling() || (mUpscaler != nullptr && !mUpscaled.isEmpty()));

        if (upscaling())
            mUpscaled.transition(commands, Use::sUndefined, Use::sAnyGeneralWrite);
#endif

        // The glare fader's query starts the frame at nothing, ahead of the trace that counts.
        mSunGlare.begin(commands);

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

        const Image* shown = &mFrame.record(commands,
            TraceRecording{
                .mVisibility = mPass.get(),
                .mComposite = &mComposite,
                .mSpriteBin = &mSpriteBin,
                .mSpriteShade = &mSpriteShade,
                .mInputs = inputs,
                .mBinSlot = mRing.getRecordingSlot(),
                .mBuffers = mWorld.mBuffers.get(),
                .mAsked = camera,
                .mSampled = sampled,
                .mCounts = &frame.mHitCount,
                .mTarget = &claimTarget(),
                .mSum = mSum.isEmpty() ? nullptr : &mSum,
                .mAccumulate = options.mAccumulate,
                .mAirLost = airLost,
                .mHistoryLost = historyLost,
                .mFilter = filtering,
                .mTimer = &timer,
            });

#ifdef OPENMW_RTX_DLSS
        if (upscaling())
        {
            historyRead = true;
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
                    .mOutput = mUpscaled,
                    .mJitter = sampled.mCamera.mJitter,
                    .mFrameDeltaMs = sinceLastMs,
                    .mReset = historyLost,
                });

            // What NGX recorded is its own; nothing here knows which stages it used.
            mUpscaled.transition(commands, Use::sAnyGeneralWrite, Use::sTraceReadWrite);

            timer.close(commands);
            shown = &mUpscaled;
        }
#endif

        // The puffs over the reconstructed frame, at its own extent, and then the frame is what
        // the lens spreads and the curve maps. The bloom samples what this leaves, rather than
        // loading it — `BloomPass` binds the frame as a combined image sampler — so the scope
        // after it names both reads.
        assert(shown == inputs.mShown && "the puffs composited over a frame the set does not name");
        if (!upscaling())
            shown->transition(commands, Use::sAnyGeneralRead, Use::sTraceReadWrite);
        mPass->recordSpriteComposite(commands, inputs, channels, frame.mHitCount, sampled.mFrame,
            VkExtent2D{ shown->getWidth(), shown->getHeight() }, &timer);
        shown->transition(commands, Use::sTraceReadWrite, Use::sComputeReadOrSample);

        // What the lens will spread, built here and applied by the curve. Nothing is
        // written back over the frame — `BloomPass` says why the trace's own answer has to
        // reach `readComposite` untouched.
        timer.open(commands, "bloom");
        mBloom.record(commands, *shown);
        timer.close(commands);

        // Measured off the image the curve is about to map, which is the upscaled one
        // wherever something upscales — see `histogram.comp` for what measuring the other one
        // costs. One `shown` feeds both, so the two cannot come apart.
        timer.open(commands, "exposure");
        if (options.mExposure.has_value())
            mExposure.recordFixed(commands, *options.mExposure);
        else
        {
            // The third thing that reads a lost history, and the only one that reads it on
            // every frame: the eye has no past to adapt from either.
            historyRead = true;
            mExposure.record(commands, *shown, 0.001f * sinceLastMs, historyLost, options.mExposureBias);
        }
        timer.close(commands);

        // What the eye saw of the sun's quad, eased at the query's own rate, which the curve
        // lays the glare fader over the picture by. Read after the trace and before the curve,
        // on the device: a frame's own count is a frame's own wash.
        timer.open(commands, "glare");
        mSunGlare.record(commands, 0.001f * sinceLastMs, historyLost);
        timer.close(commands);

        timer.open(commands, "tone");
        mTone->record(commands, *shown, mExposure.getExposure(), mSunGlare.getShare(),
            channels.get(Channel::StarsShown), mBloom.getPyramid(), inputs.mTextures, mTargets.current(),
            toneFor(sampled, mOutputWidth, mOutputHeight, channels.getWidth(), channels.getHeight()));
        timer.close(commands);

        recordDebugLines(commands, frame, sampled, channels, options.mDebug, timer);

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

        // Held and not drained: a picture of it recorded this frame and not yet carried rides the
        // next submit, and so does the last placement's refit, so the scene goes once the timeline
        // has passed that submit and no sooner — `DyingScene` says why that is the graveyard's
        // rule. A drain here idled the whole device every time the inventory closed.
        mDyingScenes.push_back(DyingScene{
            .mUntil = mDevice.getTimeline().getNext(),
            .mScene = std::move(mViewScenes[scene.getViewIndex()]),
        });
        mFreeViewScenes.free(scene.getViewIndex());
    }

    void VulkanRenderer::growViewTargets(std::uint32_t width, std::uint32_t height)
    {
        if (mView.holds(width, height))
            return;

        // The one drain a picture still pays, and only the first picture of a new size pays it.
        finishTraces();

        mView.grow(width, height, mProfile.mRadianceWidth);

        mViewTarget = Image(mDevice, mView.getWidth(), mView.getHeight(), PresentTargets::sFormat,
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

        ViewScene& traced = sceneAt(options.mScene);

        VisibilityInputs inputs = describeInputs(traced, &mView.getFogVolume(), camera.mRayMask);
        inputs.mShown = &mView.getColour();

        // The scene's own, filled here rather than by the caller, because whether the scene behind
        // a camera holds a cloud is this renderer's to answer. The only one of `sampleCamera`'s
        // fields a picture wants. Copied because the caller's block is theirs.
        Shaders::VisibilityConstants sampled = camera;
        sampled.mArmsSpread = armsSpreadOf(camera);
        const InstanceCounts& counts = traced.mAcceleration->getInstanceCounts();
        sampled.mMediumInFrame = counts.mMedium > 0 ? 1 : 0;
        sampled.mAdditiveInFrame = counts.mAdditive > 0 ? 1 : 0;
        sampled.mArmsInFrame = counts.mFirstPerson > 0 && (camera.mRayMask & Shaders::MASK_FIRST_PERSON) != 0 ? 1 : 0;

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
                    .mAsked = camera,
                    .mSampled = sampled,
                    .mCounts = &mViewCounts,
                    .mTarget = &mViewTarget,
                });

            // The puffs over the picture, at its own extent: a torch's flame in a doll's hand is a
            // sprite.
            mView.getColour().transition(commands, Use::sAnyGeneralRead, Use::sTraceReadWrite);
            mPass->recordSpriteComposite(commands, inputs, channels, mViewCounts, sampled.mFrame,
                VkExtent2D{ options.mWidth, options.mHeight }, nullptr);
            mView.getColour().transition(commands, Use::sTraceReadWrite, Use::sComputeReadOrSample);

            // One, and measured off nothing, or the same armour would be a different brightness
            // in two windows; out of its own buffer, for what `ExposurePass::getPictureExposure`
            // says. And no lens, because a map tile is a diagram.
            mTone->record(commands, mView.getColour(), mExposure.getPictureExposure(), mSunGlare.getNoShare(),
                channels.get(Channel::StarsShown), nullptr, inputs.mTextures, mViewTarget,
                toneFor(camera, options.mWidth, options.mHeight, channels.getWidth(), channels.getHeight()));

            mViewTarget.transition(commands, Use::sComputeWrite, Use::sCopyRead);

            // Borrowed rather than transitioned. Where a GUI texture rests between writes is
            // `GuiTextures`' to say, and a caller that said it here had to keep a barrier's scope in
            // step with the commands below — which it did not.
            mGuiTextures.writeWith(texture, commands, [&](const Image& into, VkImageLayout layout) {
                assert(options.mWidth <= into.getWidth() && options.mHeight <= into.getHeight());

                // Cleared whole and then covered in part, and only where the picture does not
                // cover it all: what the trace fills is as much of the texture as the widget is
                // currently wide, and the rest has to be the clear colour rather than what a wider
                // picture left there the last time this was drawn.
                // Both are transfer writes to the same image and nothing orders two of those, so
                // the clear is left as what the copy meets.
                if (options.mWidth < into.getWidth() || options.mHeight < into.getHeight())
                    into.clear(commands, Use::sTransferWrite,
                        VkClearColorValue{
                            .float32 = { options.mClear[0], options.mClear[1], options.mClear[2], options.mClear[3] } },
                        Use::sCopyWrite);

                assert(layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                    && "a texture lent in another layout than a copy takes");
                mViewTarget.copyTo(commands, into, layout, VkExtent2D{ options.mWidth, options.mHeight });
            });

            if (options.mReadBack)
                mGuiTextures.readBackWith(texture, commands, mRing.getRecording());
        }
        trace.defer();

        // The value the batch rides: the next submit this pool makes, whichever that is. The
        // tables it reads were named the same value as they were handed out above.
        traced.mPictureRides[traced.mSlot.get()] = mDevice.getTimeline().getNext();
    }

    bool VulkanRenderer::takeGuiCopy(const GuiSlot texture, const std::span<std::uint8_t> into)
    {
        return mGuiTextures.takeCopy(texture, into, mRing.getFinished());
    }

    void VulkanRenderer::finishGuiTraces()
    {
        finishTraces();
        mGuiTextures.landTraces();
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
        frame.read(mPool, VK_IMAGE_LAYOUT_GENERAL, pixels);
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
        image.read(mPool, VK_IMAGE_LAYOUT_GENERAL, bytes);

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
