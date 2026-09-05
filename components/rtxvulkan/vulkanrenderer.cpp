#include "vulkanrenderer.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <utility>

#include <components/rtx/camera.hpp>
#include <components/rtx/error.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/gbuffer.h>

#include "gbuffer.hpp"
#include "image.hpp"
#include "micromappass.hpp"
#include "physicaldevice.hpp"
#include "pipelinecache.hpp"
#include "presenter.hpp"
#include "requirements.hpp"
#include "result.hpp"
#include "sceneacceleration.hpp"
#include "scenebuffers.hpp"
#include "scenemicromaps.hpp"
#include "skintables.hpp"
#include "texture.hpp"
#include "visibilitypass.hpp"

#ifdef OPENMW_RTX_DLSS
#include "dlss.hpp"
#include "dlsspass.hpp"
#endif

namespace Rtx
{
    namespace
    {
        /// One half float, as the number it stands for.
        ///
        /// **By bits, where the test harness spells the same conversion out by arithmetic.** That is
        /// deliberate on both sides: a test that decoded a half through the helper the renderer used
        /// would agree with it however wrong it was, so the two derivations are kept apart and each
        /// checks the other.
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

        /// The bounce resolved: the temporal mean, and then the cascade over it.
        ///
        /// **The barrier between them is the reason this is one call.** Two compute dispatches are
        /// unordered inside a command buffer, so the cascade reads what the accumulator wrote only
        /// where something says so — and the picture-inside-the-interface copy of this chain said
        /// nothing at all.
        ///
        /// @param timer null where the run is not being timed, which a picture is not.
        const Image& recordDenoise(VkCommandBuffer commands, const GBuffer& channels, AccumulatePass& accumulate,
            const AtrousPass& filter, const Shaders::Camera& camera, const float far, const bool historyLost,
            GpuTimer* const timer)
        {
            // **The temporal half first, and the cascade is what fills in where it was rejected.**
            // The accumulator replaces the trace's single sample with the mean of the frames this
            // surface has been seen over, and hands on the variance of that mean — which is what
            // lets the levels below stop at an edge in the light rather than only at an edge in the
            // geometry.
            openZone(timer, commands, "accumulate");
            const Image& moments = accumulate.record(commands, channels, camera, far, historyLost);
            const Image& blended = accumulate.getBlended();
            closeZone(timer, commands);

            // The cascade reads what the accumulator just wrote, in both images. The history it
            // writes for the next frame is ordered by the discard `AccumulatePass::record` made of
            // it, which named a compute write as what would come next.
            for (const Image* written : { &blended, &moments })
                written->transition(commands, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

            openZone(timer, commands, "filter");
            const Image& indirect
                = filter.record(commands, channels, blended, moments, accumulate.getHistory(), camera);
            closeZone(timer, commands);

            return indirect;
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
        /// Whether the frame has a sea to synthesise.
        ///
        /// **The shader's own test**, so the two cannot disagree: a cell with no water carries a
        /// level of minus infinity, every "how deep" comes out never positive, and nothing samples
        /// the wave tiles — which is what makes not building them for that frame free. Every
        /// interior is such a frame, and the synthesis was a fifth of a millisecond of device time
        /// in each of them.
        bool hasSea(const Shaders::VisibilityConstants& frame)
        {
            return !std::isinf(frame.mWaterLevel);
        }

        /// The display pass's own description of the frame.
        ///
        /// **The camera is the trace's basis on the picture's grid.** `rayAt` divides by the camera's
        /// own extent, so handing it the output's is what turns an output pixel into the ray it
        /// shows — and the jitter goes, because a jitter is what lets several traced frames average
        /// into one and this pass draws once. The spread angle comes down with the pixel: a pixel of
        /// the picture subtends what one of the trace's did, times how much smaller it is.
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
        , mReorder(options.mReorder)
        , mUpscale(options.mUpscale)
        , mPreset(options.mPreset)
        , mChannelLayout(GBuffer::describeLayout(mDevice))
        , mFogVolumeLayout(FogVolume::describeLayout(mDevice))
        , mAccumulate(mDevice, options.mShaderDirectory)
        , mFilter(mDevice, options.mShaderDirectory)
        , mViewAccumulate(mDevice, options.mShaderDirectory)
        , mViewFilter(mDevice, options.mShaderDirectory)
        , mComposite(mDevice, mPool, options.mShaderDirectory)
        , mBloom(mDevice, options.mShaderDirectory)
        , mWaves(mDevice, mPool, options.mShaderDirectory)
        , mFog(mDevice, mPool)
        , mExposure(mDevice, options.mShaderDirectory)
        , mSkinPass(mDevice, options.mShaderDirectory)
        , mSpriteBin(mDevice, options.mShaderDirectory)
        , mGuiPass(mDevice, options.mShaderDirectory, sTargetFormat)
        , mGuiTextures(mDevice, mPool)
    {
        // Before the first targets, because what to trace at is its answer and not ours.
        if (mUpscale != Upscale::Off)
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
        return mNgx != nullptr && mUpscale != Upscale::Off;
#else
        return false;
#endif
    }

    void VulkanRenderer::setUpscale(Upscale upscale)
    {
        if (upscale == mUpscale)
            return;

        // **Before anything is torn down**, so a mode this machine cannot reach leaves the renderer
        // drawing exactly as it was rather than half way between two of them.
        if (upscale != Upscale::Off)
            startUpscaler();

        mUpscale = upscale;

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

        // Whatever upscales picks the render size; without one the two extents are the same number
        // twice, and every pass below is written as though they always might not be.
        //
        // **The mode and not the runtime, which stopped being the same question the moment the mode
        // could change.** A runtime that is up because somebody upscaled and then turned it off is
        // still up — it costs a quarter of a second to raise and is kept for the next time — and
        // asking it what to trace at for no upscaling at all is a question it refuses.
        VkExtent2D render{ width, height };
#ifdef OPENMW_RTX_DLSS
        if (upscaling())
            render = mNgx->getRenderSize(VkExtent2D{ width, height }, mUpscale);
#endif
        mRenderWidth = render.width;
        mRenderHeight = render.height;

        // `SAMPLED` because an upscaler samples what it is handed, and one bit short of that is a
        // black frame nothing reports. See `GBuffer`, which carries it for the same reason.
        // `TRANSFER_SRC` because `Channel::Radiance` copies this out: it is the frame a measurement
        // is taken on, where `readPixels` gives the one a display would show.
        mColour = std::make_unique<Image>(mDevice, mRenderWidth, mRenderHeight, VK_FORMAT_R32G32B32A32_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "colour");

        // **Two, and interchangeable**, because the frame after this one must not rewrite the image
        // the present is still blitting out of. They swap roles every present; anything that told
        // them apart would break the frame they swapped on.
        const auto makeTarget = [&](const char* name) {
            return std::make_unique<Image>(mDevice, mOutputWidth, mOutputHeight, sTargetFormat,
                // Drawn into as well as written: the tone curve writes it as a storage image and the
                // GUI rasterises over what that left.
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                    | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                name);
        };

        // Numbered rather than named: which one is being written changes every present, so a name
        // that said so would be wrong on half the frames it appeared in.
        mTarget = makeTarget("target 0");
        mSpare = makeTarget("target 1");
        mPresented = nullptr;

        // **Black and in `GENERAL` from the moment they exist.** Everything that reads a target
        // expects that layout, and the GUI is drawn over one whether or not a frame has been traced
        // into it — a main menu and a loading screen have no world behind them.
        mPool.submitAndWait([&](VkCommandBuffer commands) {
            const VkClearColorValue black{ .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
            const VkImageSubresourceRange whole{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

            for (const Image* target : { mTarget.get(), mSpare.get() })
            {
                target->transition(commands, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);

                vkCmdClearColorImage(
                    commands, target->getHandle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &whole);

                target->transition(commands, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
            }
        });

        mChannels = std::make_unique<GBuffer>(mDevice, mChannelLayout, mRenderWidth, mRenderHeight);
        mFogVolume = std::make_unique<FogVolume>(mDevice, mPool, mFogVolumeLayout, mRenderWidth, mRenderHeight);
        mAccumulate.resize(mRenderWidth, mRenderHeight);
        mFilter.resize(mRenderWidth, mRenderHeight);

#ifdef OPENMW_RTX_DLSS
        // Released before the next is built: the feature holds the network's weights for one pair
        // of resolutions, which is most of what it occupies.
        //
        // **And the image it writes goes with it**, which is what makes `upscaling()` the answer for
        // both. It is sized to the output — 33 MiB at 1080p and 133 at 4K — so leaving it behind
        // held that memory until something upscaled again, at the extent of whichever frame last
        // did, and every frame between the two still discarded it.
        mUpscaler.reset();
        mUpscaled.reset();

        if (upscaling())
        {
            mUpscaled = std::make_unique<Image>(mDevice, mOutputWidth, mOutputHeight, VK_FORMAT_R32G32B32A32_SFLOAT,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, "upscaled");

            // Building uploads the network's weights, which is once per resolution rather than
            // once per frame.
            mPool.submitAndWait([&](VkCommandBuffer commands) {
                mUpscaler = std::make_unique<DlssPass>(
                    *mNgx, commands, render, VkExtent2D{ mOutputWidth, mOutputHeight }, mUpscale, mPreset);
            });
        }
#endif

        // **Over whatever the frame is by the time the curve maps it**, which is the upscaler's
        // output where one runs and the trace's own extent where none does. The same test the frame
        // path makes, because a pyramid built at the other extent is a bloom at the wrong scale.
        const std::uint32_t shownWidth = upscaling() ? mOutputWidth : mRenderWidth;
        const std::uint32_t shownHeight = upscaling() ? mOutputHeight : mRenderHeight;
        mBloom.resize(shownWidth, shownHeight);

        // A frame of a different size is not one this one can be reprojected against.
        mPreviousCamera = Shaders::VisibilityConstants{};

        // **Dropped rather than resized, because most runs never make one.** Sixteen bytes a pixel
        // is 33 MiB at 1080p and 133 MiB at 4K, and it buys a sum that neither rounds nor clips —
        // which is worth every byte to the reference mode and nothing at all to the frame a window
        // or a plain shot draws. The first averaging frame is what asks for it.
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
            // one. This used to build a `Dlss` of its own to ask with and let it go again; NGX keeps
            // one runtime per process and its shutdown is unconditional, so that second one ended
            // this renderer's the moment it left scope — and what that looked like was the upscaler
            // refusing a frame a cell load later with `FAIL_NotInitialized`, pointing at nothing.
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

    const VulkanRenderer::ViewScene& VulkanRenderer::sceneAt(std::uint32_t slot) const
    {
        if (slot == sWorld)
            return mWorld;

        assert(slot < mViewScenes.size() && mViewScenes[slot] != nullptr && "a scene slot nothing holds");
        return *mViewScenes[slot];
    }

    VulkanRenderer::ViewScene& VulkanRenderer::sceneAt(std::uint32_t slot)
    {
        return const_cast<ViewScene&>(std::as_const(*this).sceneAt(slot));
    }

    VisibilityInputs VulkanRenderer::describeInputs(
        const ViewScene& held, const std::uint32_t slot, const FogVolume* const volume) const
    {
        return VisibilityInputs{
            .mScene = held.mAcceleration->getTopLevel(),
            .mBuffers = held.mBuffers.get(),
            .mSlot = slot,
            .mIndexBlocks = held.mAcceleration->getIndexBlocks(),
            .mTextures = held.mTextures->getSet(),
            .mWaves = &mWaves,
            .mFog = &mFog,
            .mFogVolume = volume,
            .mWater = held.mAcceleration->getWaterInstanceCount() > 0,
        };
    }

    void VulkanRenderer::setScene(
        std::uint32_t slot, const SceneDesc& scene, std::span<const TextureData> textures, const SeaState& sea)
    {
        ViewScene& held = sceneAt(slot);

        // **Nothing may be in flight over what is about to go.** A rebuild is a load, and a load
        // waits: for the frames tracing the old scene, and for a placement the frame being recorded
        // may have submitted without a fence of its own.
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
        held.mMicromaps.reset();

        if (slot == sWorld)
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

            // The copies are new and alike, so nothing has read either.
            mWorldSlot = 0;
            mReadBy.fill(sNeverRead);
        }

        // Made here for the same reason a frame's are: both of the two below want them, and this is
        // the only place that knows both.
        makeInstanceRecords(scene, held.mRecords);

        // **One submit for the whole cell.** Every structure, every table and every texture is
        // recorded into this and the queue is asked once, at the flush below; each of them used to
        // be its own round trip, and Balmora's are 367 of them.
        Batch setup(mPool);

        // **The world's, because there is one sea and every scene traces it.** A doll and a map tile
        // carry a sea state of their own only because they take the same argument, and letting one
        // of those redraw the spectrum would put the interface's water under the world.
        if (slot == sWorld)
            mWaves.describe(sea);

        // The world is traced by two frames at once and so keeps two copies of what a frame writes;
        // a picture inside the interface is traced and waited for, and keeps one.
        Graveyard& graveyard = mRing.recording().mGraveyard;
        const std::uint32_t slots = slot == sWorld ? sFrameSlots : 1;

        held.mAcceleration = std::make_unique<SceneAcceleration>(mDevice, scene, slots);
        held.mBuffers = std::make_unique<SceneBuffers>(mDevice, scene, held.mRecords, slots, graveyard);
        held.mSkinTables = std::make_unique<SkinTables>(mDevice, scene, slots, graveyard);

        // **The textures before the structures, because the bake between them samples the
        // masks.** The uploads are recorded first, the bake reads what they wrote, and the
        // structures are built over what it decided — three stretches of one command buffer.
        held.mTextures = std::make_unique<TextureArray>(
            mDevice, setup, static_cast<std::uint32_t>(scene.getTextures().size()), textures, graveyard);
        held.mMicromaps = std::make_unique<SceneMicromaps>(mDevice);

        // **Built once and kept, because building one compiles every kernel the trace can ever
        // need — 6.3 s on a cold cache, measured.** Nothing about the pass depends on the scene: it
        // needs the device and the shape of the texture set, and every array declares that shape
        // identically — the bindless binding is sized to its maximum rather than to the cell, so
        // what varies between scenes is how many descriptors get allocated and never what the
        // layout says. Identically defined layouts are compatible, so a set from a later array
        // binds against the pipeline layout the first one produced. `TextureArray`'s layout is
        // where that invariant is kept.
        //
        // A doll can be the first thing this renderer ever builds — a race preview stands in front
        // of a game that has no world yet — and the pass belongs to neither scene. The bake and the
        // display curve read the same array and are kept for the same reason.
        if (mPass == nullptr)
        {
            mPass = std::make_unique<VisibilityPass>(mDevice, setup, mShaderDirectory, held.mTextures->getLayout(),
                mChannelLayout, mFogVolumeLayout, mCountHits, mCountCrossings, mReorder);
            mTone = std::make_unique<TonePass>(mDevice, mPool, held.mTextures->getLayout(), mShaderDirectory);
            mMicromapPass = std::make_unique<MicromapPass>(mDevice, held.mTextures->getLayout(), mShaderDirectory);
        }

        // **Posed before it is built.** The structures are built over the first copy of the
        // positions, and a skinned body's bind pose is not where the body is; the pass writes the
        // pose into that copy and the build then reads it. The other copy is owed the same pose and
        // takes it on the first placement that writes it.
        mSkinPass.record(setup.getCommands(), scene, 0, *held.mSkinTables, held.mAcceleration->getPositions(),
            held.mBuffers->getNormals(), nullptr);
        held.mMicromaps->bake(setup, *mMicromapPass, scene, *held.mBuffers, *held.mAcceleration, *held.mTextures,
            held.mAcceleration->getEveryMesh(), nullptr, graveyard);
        held.mAcceleration->build(setup, scene, held.mRecords, *held.mMicromaps, graveyard);
        held.mBuiltMeshes = scene.getMeshRevision();

        // By hand rather than left to the destructor, so a submit that fails throws out of here
        // instead of being logged on the way past.
        setup.flush();

        if (slot == sWorld)
            readStats(held);
    }

    void VulkanRenderer::extendScene(
        std::uint32_t slot, const SceneDesc& scene, std::span<const TextureData> arrived, const SeaState& sea)
    {
        ViewScene& held = sceneAt(slot);
        assert(held.mAcceleration != nullptr && "extendScene before setScene");

        // **An arrival waits.** What arrives is written into every copy of the geometry and the
        // tables — the normals, the positions, the mesh table, the layers — and a frame still
        // reading any of them would see it torn. A cell crossing is tens of milliseconds of work
        // in any case, and the frame it lands in is not one this renderer keeps smooth. A picture
        // inside the interface is traced and waited for, so it has nothing to wait.
        //
        // **And it opens the frame it lands in, so that its builds have a zone.** The batch below
        // rides that frame's placement submit, ahead of the refit and the top level, so the bracket
        // around the builds has to be written against that frame's timer — and `beginFrame` clears
        // the timer, so a zone opened before it would be forgotten. Nothing is in flight to wait
        // for by then. A picture inside the interface opens no frame and is not timed, which is the
        // rule `placeScene` states.
        GpuTimer* timer = nullptr;
        if (slot == sWorld)
        {
            mRing.finishAll();
            timer = &mRing.begin().mTimer;
        }

        Graveyard& graveyard = mRing.recording().mGraveyard;

        Batch setup(mPool);
        held.mTextures->write(setup, arrived, graveyard);

        // **The meshes that arrived, and no others.** Everything already built stays where it is:
        // the geometry blocks are appended to rather than replaced, so every address a structure was
        // built from is still its own, and the storage a departing mesh gives back goes to the next
        // one that fits.
        //
        // **The revision and not the count.** A slot a departing cell freed is taken over by the
        // next mesh that fits, so the table can hold different geometry at the same size — and a
        // guard on the size would send that here without noticing.
        if (scene.getMeshRevision() != held.mBuiltMeshes)
        {
            held.mBuffers->extend(scene, graveyard);
            held.mSkinTables->extend(scene, graveyard);
            held.mAcceleration->extend(scene, graveyard);
            held.mMicromaps->release(scene.getFreedMeshes(), graveyard);

            // **Posed before it is built**, as `setScene` does: an actor walking in is built over
            // its pose and not over its bind. Into the first copy, which is what the build reads;
            // the placement below poses the copy the frame traces. Untimed, so the frame's report
            // carries one `skin` zone and it is the placement's. The bake is timed, as the builds
            // are: what an arrival adds to the frame it lands in is the question its zone answers.
            mSkinPass.record(setup.getCommands(), scene, 0, *held.mSkinTables, held.mAcceleration->getPositions(),
                held.mBuffers->getNormals(), nullptr);
            held.mMicromaps->bake(setup, *mMicromapPass, scene, *held.mBuffers, *held.mAcceleration, *held.mTextures,
                scene.getArrivedMeshes(), timer, graveyard);
            held.mAcceleration->buildArrived(setup, scene, *held.mMicromaps, timer, graveyard);
            held.mBuiltMeshes = scene.getMeshRevision();
        }

        // **Deferred to the placement's submit, not flushed ahead of it.** `placeScene` submits
        // what was recorded here in the same call as the refit and the top level, ahead of them,
        // and the barrier every upload and build ends in is what orders them — a build reads
        // structures the deferred half wrote as it would inside one command buffer. A composite
        // landing used to be a submit, a fence and a wait of its own on the frame it landed in, and
        // this is that round trip removed.
        setup.defer();

        // Always, because the top level names every instance and an arrival changed the list. It is
        // rebuilt every frame regardless, so an arrival costs it nothing.
        placeScene(slot, scene, sea);

        // **The history is kept.** Nothing was renumbered, so what the last frame resolved still
        // describes the same surfaces — and throwing it away is a visible flash every time an actor
        // walks into view with a texture nobody has worn yet.
        if (slot == sWorld)
            readStats(held);
    }

    std::uint32_t VulkanRenderer::getTextureCount(std::uint32_t slot) const
    {
        const ViewScene& held = sceneAt(slot);
        return held.mTextures == nullptr ? 0 : held.mTextures->getCount();
    }

    void VulkanRenderer::dropTextures(std::uint32_t slot, std::span<const std::uint32_t> textures)
    {
        ViewScene& held = sceneAt(slot);

        // Before there is an array at all, which is a scene that swept before it was ever handed
        // over. There is nothing holding the images to destroy.
        if (held.mTextures == nullptr)
            return;

        held.mTextures->drop(textures, mRing.recording().mGraveyard);
    }

    bool VulkanRenderer::recordPlacement(
        const SkinPass& skin, ViewScene& held, const SceneDesc& scene, const Placing& placing)
    {
        // **What the scene let go of, given back here.** Walking away from a ring frees its meshes
        // and nothing arrives to take them over until the walk reaches the far side of the next one,
        // so a frame that only places is the one that must not hold their structures. Already done
        // where `extendScene` came through, and asking twice costs two comparisons a slot.
        held.mAcceleration->release(scene.getFreedMeshes(), placing.mGraveyard);
        held.mMicromaps->release(scene.getFreedMeshes(), placing.mGraveyard);

        // A material rewritten under the micromap baked against it is the one thing a placement
        // cannot carry, and `SceneMicromaps::check` says why it is a throw and not a rebuild.
        held.mMicromaps->check(scene);

        // **Once, for the slots that changed, and both halves read it.** The rows carry a matrix
        // inverse apiece and a nine-by-nine exterior is fifty thousand of them; the acceleration
        // structure and the instance table were each building the whole set for themselves, every
        // frame, to change a hundred of them.
        updateInstanceRecords(scene, held.mRecords, held.mChangedRecords);

        // **The pose first, because the refit reads it.** Every skinned body and morphed face this
        // copy owes is computed into it here, and the barrier the pass ends in is what the refit
        // and the trace wait on.
        const bool posed = skin.record(placing.mCommands, scene, placing.mSlot, *held.mSkinTables,
            held.mAcceleration->getPositions(), held.mBuffers->getNormals(), placing.mTimer);

        const bool built
            = held.mAcceleration->place(scene, held.mRecords, held.mChangedRecords, *held.mMicromaps, placing);

        // **Nothing to report, because nothing here is recorded.** The tables are host-visible and
        // this writes them; what the trace reads of them is made visible by the submit that follows,
        // which is why only the halves above have a command buffer and an answer about it.
        //
        // **Only what a moving world changed**, which is the instance rows and the lights.
        // Rebuilding all of it was measured at twenty to twenty-seven milliseconds on a nine-by-nine
        // region and was the largest single cost in the frame.
        held.mBuffers->place(scene, held.mRecords, held.mChangedRecords, placing.mSlot, placing.mGraveyard);

        return posed || built;
    }

    void VulkanRenderer::placeScene(std::uint32_t slot, const SceneDesc& scene, const SeaState& sea)
    {
        ViewScene& held = sceneAt(slot);
        assert(held.mAcceleration != nullptr && "placeScene before setScene");

        // **A picture inside the interface is placed between frames and waited for.** It has one
        // copy of everything and no ring: it is neither timed nor allowed to open the frame's
        // report, and its placement is a submit of its own.
        if (slot != sWorld)
        {
            Graveyard& graveyard = mRing.recording().mGraveyard;
            mPool.submitAndWait([&](VkCommandBuffer commands) {
                recordPlacement(mSkinPass, held, scene, Placing{ .mCommands = commands, .mGraveyard = graveyard });
            });
            return;
        }

        // **A placement opens the frame, and every placement before a trace joins it.** The frame's
        // report starts here and not at the trace: placing the world is the refit and the top level,
        // and a report that began at `renderFrame` would leave them out.
        FrameSlot& frame = mRing.begin();

        // Does nothing where the sea is the one already drawn for, which is every frame but the
        // first and any on which the weather turned the wind.
        mWaves.describe(sea);

        // **The copy this placement writes is the one the last frame did not trace**, and whatever
        // frame last traced it is waited for here. Usually that frame has long since signalled —
        // the CPU is a frame ahead and no more — and the wait is a comparison; when the GPU is
        // behind, this is where the CPU stands still, which is the right place. The other copy and
        // not a parity of its own, because a frame need not place: two traces of one placement read
        // the same copy twice, and the next placement has to go where neither of them is.
        const std::uint32_t into = (mWorldSlot + 1) % sFrameSlots;
        if (mReadBy[into] != sNeverRead)
            mRing.finishThrough(mReadBy[into]);

        // **The placement's own submit, without a fence and without a wait.** A picture inside the
        // interface traced before this frame's trace needs the top level to have reached the queue;
        // the frame's fence, later on the queue, covers this submit too. Nothing recorded is
        // nothing submitted, which is every frame of a standing camera in an empty place.
        const VkCommandBuffer placement = mRing.takePlaceCommands(frame);
        mPool.begin(placement);

        if (recordPlacement(mSkinPass, held, scene,
                Placing{
                    .mCommands = placement,
                    .mSlot = into,
                    .mTimer = &frame.mTimer,
                    .mGraveyard = frame.mGraveyard,
                }))
            mPool.submit(placement, VK_NULL_HANDLE, frame.mGraveyard);
        else
            checkVk(vkEndCommandBuffer(placement), "vkEndCommandBuffer");

        mWorldSlot = into;

        readPlacedStats(held);
    }

    void VulkanRenderer::readPlacedStats(const ViewScene& held)
    {
        mStats.mInstances = held.mAcceleration->getInstanceCount();
        mStats.mCutoutInstances = held.mAcceleration->getCutoutInstanceCount();
        mStats.mMicromappedInstances = held.mAcceleration->getMicromappedInstanceCount();
        mStats.mTableBytes = held.mBuffers->getBytes() + held.mSkinTables->getBytes();
    }

    void VulkanRenderer::readStats(const ViewScene& held)
    {
        readPlacedStats(held);

        mStats.mStructureBytes = held.mAcceleration->getStructureBytes();
        mStats.mMicromapBytes = held.mMicromaps->getBytes();
        mStats.mMicromapsUntextured = held.mMicromaps->getUntexturedCount();

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
            // Same reason as the destructor's: remaking a swapchain resets the command pool, and a
            // batch handed over is sitting in it waiting for a submit.
            mGuiTextures.finish();

            // **What the swapchain came back with, not what was asked for.** A surface clamps to
            // what it can do, and targets sized to the request would then be blitted through a
            // scale nobody chose.
            mPresenter->resize(VkExtent2D{ width, height });

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

    std::uint32_t VulkanRenderer::addGuiTexture(std::uint32_t width, std::uint32_t height)
    {
        return mGuiTextures.add(width, height);
    }

    void VulkanRenderer::writeGuiTexture(
        std::uint32_t texture, const GuiRegion& region, std::span<const std::uint8_t> rgba)
    {
        mGuiTextures.write(texture, region, rgba);
    }

    std::span<std::uint8_t> VulkanRenderer::lendGuiTexture(std::uint32_t texture, const GuiRegion& region)
    {
        return mGuiTextures.lend(texture, region);
    }

    void VulkanRenderer::sendGuiTexture(std::uint32_t texture)
    {
        mGuiTextures.send(texture);
    }

    void VulkanRenderer::dropGuiTexture(std::uint32_t texture)
    {
        mGuiTextures.drop(texture);
    }

    void VulkanRenderer::drawGui(std::span<const GuiVertex> vertices, std::span<const GuiBatch> batches)
    {
        assert(mTarget != nullptr);

        if (vertices.empty() || batches.empty())
            return;

        // The interface drawn two frames ago drew out of this slot; its fence is what says the
        // vertices may be written over.
        FrameSlot& gui = mRing.slotOf(mGuiFrame);
        if (gui.mGuiPending)
        {
            awaitVk(mDevice, gui.mGuiFence, "the interface drawn two frames ago");
            gui.mGuiPending = false;
            gui.mGuiGraveyard.clear();
        }

        // **After the clear and before anything is handed over, which is what both halves of it
        // want.** A texture given back was drawn with as recently as the interface still on the
        // queue, and this frame's fence is the first one that says every one of those draws has
        // finished; the staging turns on the same signal, for the reason `GuiTextures::mStaging`
        // gives.
        mGuiTextures.startFrame(gui.mGuiGraveyard);

        gui.mGuiGraveyard.bury(
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
        mPool.begin(gui.mGuiCommands);

        const VkCommandBuffer commands = gui.mGuiCommands;
        mTarget->transition(commands, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        mGuiPass.record(commands, *mTarget, gui.mGuiVertices.getHandle(), mGuiDraws);

        // Back where everything else expects it: the presenter blits out of `GENERAL` and so
        // does a read back.
        mTarget->transition(commands, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);

        mPool.submit(commands, gui.mGuiFence, gui.mGuiGraveyard);
        gui.mGuiPending = true;
        ++mGuiFrame;
    }

    bool VulkanRenderer::presentFrame()
    {
        assert(mPresenter != nullptr && "presentFrame on a renderer that was given no window");
        assert(mTarget != nullptr);

        const bool shown = mPresenter->present(*mTarget);

        // **The next frame writes the other one.** The blit `present` queued reads this image long
        // after the call returns, and the discard at the top of a frame waits for nothing.
        mPresented = mTarget.get();
        mTarget.swap(mSpare);

        // **Here rather than at the first write, which is what makes it free.** Two presents have
        // gone by since this image was last read, so the fence is signalled and the wait returns at
        // once; asking at the first write instead would put a frame face to face with the present
        // before it, and that one can still be waiting on the presentation engine.
        mPresenter->waitForLastUse(*mTarget);

        return shown;
    }

    FrameExtents VulkanRenderer::getExtents() const
    {
        return FrameExtents{
            .mRenderWidth = mRenderWidth,
            .mRenderHeight = mRenderHeight,
            .mOutputWidth = mOutputWidth,
            .mOutputHeight = mOutputHeight,
        };
    }

    Reconstruction VulkanRenderer::renderFrame(const Shaders::VisibilityConstants& camera, const FrameOptions& options)
    {
        assert(mPass != nullptr && "renderFrame before setScene");
        assert(camera.mCamera.mWidth == mRenderWidth && camera.mCamera.mHeight == mRenderHeight
            && "the camera has to be built for the render extent; ask getExtents");

        // **Coverage and an upscaler do not meet, and the interface says so rather than the code
        // noticing.** `traceGuiTexture` is where a picture that stops where nothing was hit is
        // traced — "not the frame's chain", as `Renderer::traceGuiTexture` puts it, with nothing
        // upscaling. This path carries coverage through the alpha of `direct` and the composite, and
        // then hands the frame to NGX, which writes the upscaled image itself and was never given
        // `pInAlpha`: what came back would be the feature's alpha rather than the frame's. It is one
        // everywhere today because nothing asks for the other thing here.
        assert((camera.mTransparentBackground == 0 || mUpscale == Upscale::Off)
            && "a frame that stops where nothing was hit belongs to traceGuiTexture, which does not upscale");

        // The frame `placeScene` opened, or a new one where nothing was placed.
        FrameSlot& frame = mRing.begin();

        // **How long since the last one, which a motion vector cannot say.** A vector carries a
        // distance; how fast that was depends on the time it took, and the upscaler tunes how hard
        // it denoises against exactly that. The exposure adapts over it too.
        //
        // **Taken off the wall only where the caller has no schedule**, which is a window and the
        // game. A run that steps its world by the frame index and reads the clock for this is a run
        // whose pictures depend on how fast it drew them — `FrameOptions::mSinceLast` says the rest.
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        const float sinceLastMs = options.mSinceLast.has_value()
            ? *options.mSinceLast * 1000.0f
            : (mLastFrameAt.has_value() ? std::chrono::duration<float, std::milli>(now - *mLastFrameAt).count() : 0.0f);
        mLastFrameAt = now;

        // The count is an atomic sum over the frame, so it starts each one at nothing — and it is
        // not started at all where the trace was specialized to write nothing into it, which is the
        // other half of taking the counter out of the game: the atomic went with `COUNT_HITS`, and
        // this is the write a frame that never reads it was still paying for. Here and not where
        // the frame opened, because a picture inside the interface traced between the two adds to
        // whichever buffer it is handed.
        if (mCountHits || mCountCrossings)
            *static_cast<FrameCounts*>(frame.mHitCount.map()) = FrameCounts{};

        // **What reconstructs this frame, decided once and by one rule.** Every switch below reads
        // this rather than working the interaction out again; the same value goes back in the frame
        // result, so what a run reports and what it did are one answer.
        const Reconstruction reconstruction = Reconstruction::resolve(mUpscale,
            ReconstructionRequest{ .mFilter = options.mFilter, .mJitter = options.mJitter, .mPreset = mPreset });
        frame.mReconstruction = reconstruction;

        // The camera as the caller wrote it, plus where in the pixel this frame samples. Filled
        // here rather than by the caller because the sequence belongs to the frame index, which is
        // the renderer's to walk.
        Shaders::VisibilityConstants sampled = camera;
        if (reconstruction.mJitter)
            sampled.mCamera.mJitter = haltonJitter(camera.mFrame);

        // **Only Ray Reconstruction reads the transparency layer**, so only a frame it is about to
        // upscale hands its sprites over. Every other trace in this renderer composites them itself.
        sampled.mLayerCompositedAfter = upscaling() ? 1 : 0;

        // The scene's answer and not the camera's, for the reason `VisibilityInputs::mWater` is one:
        // a cell with no cloud in it has nothing for the medium walk to find, wherever it is looked
        // at from.
        sampled.mMediumInFrame = mWorld.mAcceleration->getMediumInstanceCount() > 0 ? 1 : 0;

        // **The one subtraction of two world points, and it happens here.** Two camera positions a
        // step apart subtract exactly in a float; the same difference taken on the device, between
        // coordinates six figures long, would be rounding.
        sampled.mCameraMotion = camera.mOrigin - mPreviousCamera.mOrigin;
        sampled.mPreviousForward = mPreviousCamera.mCamera.mForward;
        sampled.mPreviousRight = mPreviousCamera.mCamera.mRight;
        sampled.mPreviousUp = mPreviousCamera.mCamera.mUp;

        const VisibilityInputs inputs = describeInputs(mWorld, mWorldSlot, mFogVolume.get());

        // Made by the first frame that averages, and that frame is the one that fills it.
        const bool fresh = options.mAccumulate > 0 && mSum == nullptr;
        if (fresh)
            mSum = std::make_unique<Image>(
                mDevice, mRenderWidth, mRenderHeight, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, "sum");

        // **A history is worthless after a jump no motion vector can describe.** A zero basis catches
        // the frames that have no past at all — a resize, a rebuild, the first one — and nothing
        // caught the rest: walking through a door left the previous camera intact and a reprojection
        // fetched one room onto another. The stale flags are the other half, and they are the signal
        // the renderer was already being sent.
        const bool basisLost = mPreviousCamera.mCamera.mForward.length2() <= 0.0f;

        // **The trace's, which is the fog volume's**, and what zeroes the basis in the block it is
        // handed. The volume is the only thing the trace reprojects, and the motion vectors the same
        // basis produces are read by a denoiser that is told its own answer separately.
        const bool airLost = mAirStale || basisLost;

        // Both denoisers ask this one, so it is answered once.
        const bool historyLost = mDenoiserStale || basisLost;

        // **Spent by the frame that answers it, not by the frame that ends.** Only a reconstruction
        // carrying a past reads `historyLost`, and a frame with neither denoiser carries none — so
        // the signal has to survive such a frame. Cleared at the end regardless, a `resetHistory`
        // before an unfiltered frame would be dropped rather than deferred to the frame that can
        // act on it. Recorded where it is read rather than derived a second time from the switches
        // below, which is what would go quietly wrong when one of them moved.
        bool historyAnswered = false;

        GpuTimer& timer = frame.mTimer;
        const VkCommandBuffer commands = frame.mCommands;
        mPool.begin(commands);

        // All written whole before anything reads them, so none needs its contents carried over
        // from the last frame. **But the last frame may still be reading them** — the tone's
        // output is what the interface draws over and the presenter blits, the colour is what the
        // upscaler and the curve read — so the discard is sourced at everything before it on the
        // queue rather than at the top of the pipe, which would wait for nothing.
        for (const Image* image : { mColour.get(), mTarget.get() })
            image->transition(commands, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

#ifdef OPENMW_RTX_DLSS
        // `createTargets` makes the pass and its image together and releases them together, so
        // nothing below asks whether they are there.
        assert(!upscaling() || (mUpscaler != nullptr && mUpscaled != nullptr));

        if (upscaling())
            mUpscaled->transition(commands, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT);
#endif

        // The first write needs no contents and nothing to wait on; every one after reads what
        // the last left, which the queue orders and does not make visible.
        if (mSum != nullptr)
            mSum->transition(commands, fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_LAYOUT_GENERAL,
                fresh ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                fresh ? 0 : VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

        // Before the trace and outside its zone, because the sea is a function of the clock and
        // of nothing the camera does — one synthesis serves every ray of the frame. None where
        // the frame has no water: `WavePass::record` says where the tiles are left.
        if (hasSea(sampled))
        {
            timer.open(commands, "waves");
            mWaves.record(commands, sampled.mTime);
            timer.close(commands);
        }

        // **The sprite tiles are screen space, so they belong to the frame and not to the scene.**
        // Binned on the device, into the copy this frame traces — which the frame before last is
        // done with — and ahead of the trace that reads them, in its own zone.
        mWorld.mBuffers->binSprites(mSpriteBin, camera.mOrigin, camera.mCamera, camera.mSunPosition,
            Placing{
                .mCommands = commands,
                .mSlot = mWorldSlot,
                .mTimer = &timer,
                .mGraveyard = frame.mGraveyard,
            });

        mChannels->begin(commands);
        mPass->record(commands, inputs, *mChannels, frame.mHitCount, sampled, airLost, &timer);
        mChannels->handOver(commands);

        // Where the bounce ended up: the filter's last level, or the channel the trace wrote
        // when nothing filtered it. **Ray Reconstruction is itself the denoiser**, and handing
        // it a frame the wavelet already blurred is asking it to recover what was thrown away —
        // which is why `resolve` never answers with both.
        const bool filtering = reconstruction.mDenoiser == Denoiser::Wavelet;
        const Image* indirect = &mChannels->getIndirect();
        if (filtering)
        {
            indirect = &recordDenoise(
                commands, *mChannels, mAccumulate, mFilter, sampled.mCamera, sampled.mFar, historyLost, &timer);
            historyAnswered = true;
        }

        timer.open(commands, "composite");
        mComposite.record(commands, *mChannels, *indirect, mSum.get(), *mColour,
            Shaders::CompositeConstants{
                .mWidth = mRenderWidth,
                .mHeight = mRenderHeight,
                .mAccumulate = options.mAccumulate,
            });
        timer.close(commands);

        // Whatever comes next reads what the composite just wrote.
        mColour->transition(commands, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);

        const Image* shown = mColour.get();

#ifdef OPENMW_RTX_DLSS
        if (upscaling())
        {
            timer.open(commands, "upscale");
            mUpscaler->record(commands,
                DlssInputs{
                    .mColour = *mColour,
                    .mDiffuseAlbedo = mChannels->getAlbedo(),
                    .mSpecularAlbedo = mChannels->getSpecular(),
                    .mNormalRoughness = mChannels->getGuide(),
                    .mDepth = mChannels->getDepth(),
                    .mMotion = mChannels->getMotion(),
                    .mReflectionMotion = mChannels->getReflectionMotion(),
                    .mParticleMask = mChannels->getParticleMask(),
                    .mTransparency = mChannels->getTransparency(),
                    .mTransparencyOpacity = mChannels->getTransparencyOpacity(),
                    .mTransparencyMotion = mChannels->getTransparencyMotion(),
                    .mBiasMask = mChannels->getBiasMask(),
                    .mOutput = *mUpscaled,
                    .mJitter = sampled.mCamera.mJitter,
                    .mFrameDeltaMs = sinceLastMs,
                    .mReset = historyLost,
                });
            historyAnswered = true;

            // What NGX recorded is its own; nothing here knows which stages it used.
            mUpscaled->transition(commands, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

            timer.close(commands);
            shown = mUpscaled.get();
        }
#endif

        // **What the lens will spread, built here and applied by the curve.** Nothing is
        // written back over the frame — `BloomPass` says why the trace's own answer has to
        // reach `Channel::Radiance` untouched.
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
            mExposure.record(commands, *shown, 0.001f * sinceLastMs, historyLost, options.mExposureBias);
            historyAnswered = true;
        }
        timer.close(commands);

        timer.open(commands, "tone");
        mTone->record(commands, *shown, mExposure.getExposure(), mChannels->getStarsShown(), mBloom.getPyramid(),
            inputs.mTextures, *mTarget,
            toneFor(sampled, mOutputWidth, mOutputHeight, mChannels->getWidth(), mChannels->getHeight()));
        timer.close(commands);

        // **Submitted and not waited for.** The fence is what the frame after next waits on
        // before it writes over this frame's copy of the tables, and `finishFrame` is where the
        // count and the report come back — a frame late, which is the point.
        mReadBy[mWorldSlot] = mRing.getRecording();
        mRing.submit(frame);

        // What the next frame reprojects against, and the camera as the caller gave it: a jitter is
        // where inside a pixel this frame sampled, not where the eye was.
        mPreviousCamera = camera;

        // **The air's is spent here and unconditionally, because the trace above always ran.** A
        // frame that filled the volume was told; a frame with no volume to fill has nothing to keep
        // a stale flag for, and holding it would zero the basis — and so every motion vector — for
        // as long as the player stayed indoors.
        mAirStale = false;

        if (historyAnswered)
            mDenoiserStale = false;

        return reconstruction;
    }

    std::uint32_t VulkanRenderer::addViewScene()
    {
        if (!mFreeViewScenes.empty())
        {
            const std::uint32_t slot = mFreeViewScenes.back();
            mFreeViewScenes.pop_back();
            mViewScenes[slot] = std::make_unique<ViewScene>();
            return slot;
        }

        mViewScenes.push_back(std::make_unique<ViewScene>());
        return static_cast<std::uint32_t>(mViewScenes.size() - 1);
    }

    void VulkanRenderer::dropViewScene(std::uint32_t scene)
    {
        assert(scene < mViewScenes.size() && mViewScenes[scene] != nullptr && "a scene given back twice");

        // **What a picture's placement buried is this scene's**, and the frame it was buried under
        // need never be traced — so it is given back here rather than to a scene that has gone.
        mDevice.waitIdle();
        mRing.finishAll();
        mRing.emptyGraveyards();

        mViewScenes[scene].reset();
        mFreeViewScenes.push_back(scene);
    }

    void VulkanRenderer::growViewTargets(std::uint32_t width, std::uint32_t height)
    {
        if (mViewTarget != nullptr && width <= mViewWidth && height <= mViewHeight)
            return;

        // Each axis to the larger of what was there and what is wanted, so a wide picture after a
        // tall one does not throw the tall one's height away and build it again next time.
        mViewWidth = std::max(mViewWidth, width);
        mViewHeight = std::max(mViewHeight, height);

        mViewColour = std::make_unique<Image>(
            mDevice, mViewWidth, mViewHeight, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, "view colour");

        mViewTarget = std::make_unique<Image>(mDevice, mViewWidth, mViewHeight, sTargetFormat,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "view target");

        mViewChannels = std::make_unique<GBuffer>(mDevice, mChannelLayout, mViewWidth, mViewHeight);
        mViewFogVolume = std::make_unique<FogVolume>(mDevice, mPool, mFogVolumeLayout, mViewWidth, mViewHeight);
        mViewAccumulate.resize(mViewWidth, mViewHeight);
        mViewFilter.resize(mViewWidth, mViewHeight);
    }

    void VulkanRenderer::traceGuiTexture(
        std::uint32_t texture, const Shaders::VisibilityConstants& camera, const GuiTraceOptions& options)
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

        // **Every frame in flight first.** A picture of the world binds the world's tables and
        // bins its sprites into them, and the frame that last traced them may still be reading;
        // a picture of a subject has tables of its own, but the pass, the sea and the fog below
        // are the frame's neighbours. A stall here is a picture's cost and not a frame's.
        mRing.finishAll();

        ViewScene& traced = sceneAt(options.mScene);

        // A doll and a map trace the same shader, so they need the same list — against their own
        // camera, which is not the frame's.
        const std::uint32_t slot = options.mScene == sWorld ? mWorldSlot : 0;

        const VisibilityInputs inputs = describeInputs(traced, slot, mViewFogVolume.get());

        // **The scene's own, filled here rather than by the caller.** A doll and a map tile are
        // handed constants that describe a camera, and whether the scene behind that camera holds a
        // cloud is this renderer's to answer. Copied because the caller's block is theirs.
        Shaders::VisibilityConstants shown = camera;
        shown.mMediumInFrame = traced.mAcceleration->getMediumInstanceCount() > 0 ? 1 : 0;

        // **Not counted, and not timed.** The hit count and the frame report are the frame's; a
        // picture drawn between two of them would overwrite both. The buffer is still bound because
        // the shader writes it whatever anyone does with the number, and it is the frame's, which
        // `renderFrame` zeroes before it counts.
        mPool.submitAndWait([&](VkCommandBuffer commands) {
            for (const Image* image : { mViewColour.get(), mViewTarget.get() })
                image->transition(commands, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

            if (hasSea(camera))
                mWaves.record(commands, camera.mTime);

            traced.mBuffers->binSprites(mSpriteBin, camera.mOrigin, camera.mCamera, camera.mSunPosition,
                Placing{ .mCommands = commands, .mSlot = slot, .mGraveyard = mRing.recording().mGraveyard });

            mViewChannels->begin(commands);
            mPass->record(commands, inputs, *mViewChannels, mRing.recording().mHitCount, shown, true, nullptr);
            mViewChannels->handOver(commands);

            // A doll and a map tile are one frame with no frame before them, so the accumulator is
            // a pass-through that says so: no history, and the largest variance there is, which is
            // what tells the cascade to filter as widely as it can.
            const Image& indirect = recordDenoise(
                commands, *mViewChannels, mViewAccumulate, mViewFilter, camera.mCamera, camera.mFar, true, nullptr);

            mComposite.record(commands, *mViewChannels, indirect, nullptr, *mViewColour,
                Shaders::CompositeConstants{
                    .mWidth = options.mWidth,
                    .mHeight = options.mHeight,
                });

            mViewColour->transition(commands, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

            // **One, and measured off nothing.** A picture inside the interface is looked at beside
            // the widgets around it, and an exposure that drifted with what the doll was wearing
            // would make the same armour a different brightness in two windows.
            mExposure.recordFixed(commands, 1.0f);
            // **No lens on a picture inside the interface.** A map tile is a diagram and a doll is
            // looked at beside the widgets around it, and neither is a frame the pyramid was built
            // over — `TonePass::record` reads a null one as no veil.
            mTone->record(commands, *mViewColour, mExposure.getExposure(), mViewChannels->getStarsShown(), nullptr,
                inputs.mTextures, *mViewTarget,
                toneFor(
                    camera, options.mWidth, options.mHeight, mViewChannels->getWidth(), mViewChannels->getHeight()));

            mViewTarget->transition(commands, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

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
                    into.transition(commands, layout, layout, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                }

                const VkImageCopy region{
                    .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                    .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                    .extent = { options.mWidth, options.mHeight, 1 },
                };
                vkCmdCopyImage(commands, mViewTarget->getHandle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    into.getHandle(), layout, 1, &region);
            });
        });
    }

    void VulkanRenderer::readGuiTexture(std::uint32_t texture, std::vector<std::uint8_t>& pixels)
    {
        mGuiTextures.read(texture, pixels);
    }

    void VulkanRenderer::readPixels(std::vector<std::uint8_t>& pixels)
    {
        assert(mTarget != nullptr);

        // **The frame that was finished, not the one the next will be written into.** A present has
        // already swapped those two; with no window nothing presents, nothing swaps, and the frame
        // just written is still the one `mTarget` names.
        const Image& frame = mPresented != nullptr ? *mPresented : *mTarget;
        frame.read(mPool, VK_IMAGE_LAYOUT_GENERAL, pixels);
    }

    void VulkanRenderer::readChannel(Channel channel, std::vector<float>& values)
    {
        assert(mChannels != nullptr);

        // **A switch and not a pair of ternaries**, so that a channel added and not handled here is
        // a compiler warning rather than a read of whichever one the last `else` happened to name.
        const Image* image = nullptr;
        switch (channel)
        {
            case Channel::Motion:
                image = &mChannels->getMotion();
                break;
            case Channel::Depth:
                image = &mChannels->getDepth();
                break;
            case Channel::ReflectionMotion:
                image = &mChannels->getReflectionMotion();
                break;
            case Channel::ParticleMask:
                image = &mChannels->getParticleMask();
                break;
            case Channel::TransparencyMotion:
                image = &mChannels->getTransparencyMotion();
                break;
            case Channel::BiasMask:
                image = &mChannels->getBiasMask();
                break;
            case Channel::Indirect:
                image = &mChannels->getIndirect();
                break;
            case Channel::Accumulated:
                // The denoiser's own, so a frame nothing denoised has no answer here — `getBlended`
                // asserts on one rather than handing back whatever the allocation held.
                image = &mAccumulate.getBlended();
                break;
            case Channel::Radiance:
                // The one channel that is not the trace's: the composite's own output, which is the
                // frame every other channel was gathered to make.
                image = mColour.get();
                break;
        }

        assert(image != nullptr && "a channel with no image behind it");

        std::vector<std::uint8_t> bytes;
        image->read(mPool, VK_IMAGE_LAYOUT_GENERAL, bytes);

        // **A caller asked for floats, and not every channel is stored as one.** The masks hold a
        // yes or a no in a byte, so what comes back is widened rather than reinterpreted — a
        // `memcpy` over those would hand back four texels read as one number.
        if (image->getFormat() == GBUFFER_MASK)
        {
            values.resize(bytes.size());
            for (std::size_t at = 0; at < bytes.size(); ++at)
                values[at] = static_cast<float>(bytes[at]) / 255.0f;

            return;
        }

        // **And the denoiser's blend is half floats**, which a `memcpy` would hand back as pairs of
        // halves read as one number apiece. Tested on the format rather than on a macro, because
        // four of them across three headers name this one.
        if (image->getFormat() == VK_FORMAT_R16G16B16A16_SFLOAT)
        {
            values.resize(bytes.size() / sizeof(std::uint16_t));
            for (std::size_t at = 0; at < values.size(); ++at)
            {
                std::uint16_t half = 0;
                std::memcpy(&half, bytes.data() + at * sizeof(half), sizeof(half));
                values[at] = fromHalf(half);
            }

            return;
        }

        values.resize(bytes.size() / sizeof(float));
        std::memcpy(values.data(), bytes.data(), bytes.size());
    }

    void VulkanRenderer::takeValidationErrors(std::vector<std::string>& errors)
    {
        errors.clear();

        ValidationLog* log = mInstance.getValidationLog();
        if (log == nullptr)
            return;

        for (const ValidationMessage& message : log->getErrorsOnThisThread())
            errors.push_back(message.mText);

        log->clear();
    }

    std::unique_ptr<Renderer> createVulkanRenderer(const RendererOptions& options, std::string& reason)
    {
        // **Where a machine that cannot run this backend stops, and nothing else.** No loader, no
        // driver, a device that does not qualify, no DLSS: every caller wants those as an answer
        // rather than as an unwind, and `Unsupported` is what says a failure is one of them.
        //
        // **A contract is let out.** `Error` on its own means this code broke one — a format with no
        // recorded texel size, a shader the build did not write — and the harness turned every one
        // of those into a skip, so a GPU suite could report success after it ran nothing.
        try
        {
            return std::make_unique<VulkanRenderer>(options);
        }
        catch (const Unsupported& obstacle)
        {
            reason = obstacle.what();
            return nullptr;
        }
    }
}
