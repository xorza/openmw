#include "vulkanrenderer.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>

#include <components/rtx/camera.hpp>
#include <components/rtx/error.hpp>
#include <components/rtx/scenedesc.hpp>

#ifdef OPENMW_RTX_DLSS
#include "dlss.hpp"
#include "dlsspass.hpp"
#endif

#include <components/rtx/shaders/gbuffer.h>

#include "gbuffer.hpp"
#include "image.hpp"
#include "physicaldevice.hpp"
#include "presenter.hpp"
#include "requirements.hpp"
#include "result.hpp"
#include "sceneacceleration.hpp"
#include "scenebuffers.hpp"
#include "texture.hpp"
#include "visibilitypass.hpp"

namespace Rtx
{
    namespace
    {
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

    VulkanRenderer::Frame::Frame(const Device& device, CommandPool& pool)
        : mTimer(device)
        , mHitCount(device, sizeof(std::uint32_t),
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        , mGraveyard(device, pool)
        , mGuiGraveyard(device, pool)
    {
    }

    VulkanRenderer::VulkanRenderer(const RendererOptions& options)
        : mInstance(instanceOptionsFor(options))
        , mDevice(mInstance, PhysicalDevice::select(mInstance.getHandle()), deviceExtensionsFor(options))
        , mPool(mDevice)
        , mFrames{ { Frame{ mDevice, mPool }, Frame{ mDevice, mPool } } }
        , mShaderDirectory(options.mShaderDirectory)
        , mCountHits(options.mCountHits)
        , mUpscale(options.mUpscale)
        , mPreset(options.mPreset)
        , mChannelLayout(mDevice)
        , mAccumulate(mDevice, options.mShaderDirectory)
        , mFilter(mDevice, options.mShaderDirectory)
        , mViewAccumulate(mDevice, options.mShaderDirectory)
        , mViewFilter(mDevice, options.mShaderDirectory)
        , mComposite(mDevice, mPool, options.mShaderDirectory)
        , mBloom(mDevice, options.mShaderDirectory)
        , mWaves(mDevice, mPool, options.mShaderDirectory)
        , mFog(mDevice, mPool)
        , mExposure(mDevice, options.mShaderDirectory)
        , mGuiPass(mDevice, options.mShaderDirectory, sTargetFormat)
        , mGuiTextures(mDevice, mPool)
    {
        // Three command buffers a frame — the placement's, the trace's, the interface's — allocated
        // once and recorded into again, and a fence for each of the two that are waited on.
        const std::vector<VkCommandBuffer> commands = mPool.allocate(3 * sFrameSlots);
        const VkFenceCreateInfo fence{ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        for (std::uint32_t slot = 0; slot < sFrameSlots; ++slot)
        {
            Frame& frame = mFrames[slot];
            frame.mPlaceCommands = commands[3 * slot];
            frame.mCommands = commands[3 * slot + 1];
            frame.mGuiCommands = commands[3 * slot + 2];
            checkVk(vkCreateFence(mDevice.getHandle(), &fence, nullptr, &frame.mFence), "vkCreateFence");
            checkVk(vkCreateFence(mDevice.getHandle(), &fence, nullptr, &frame.mGuiFence), "vkCreateFence");
        }

        // Before the first targets, because what to trace at is its answer and not ours.
        if (mUpscale != Upscale::Off)
        {
#ifdef OPENMW_RTX_DLSS
            mNgx = std::make_unique<Dlss>(mDevice, mInstance.getHandle());
            if (!mNgx->isAvailable())
                throw Error("DLSS Ray Reconstruction was asked for and " + mNgx->getObstacle());
#else
            // **Named rather than quietly ignored.** A build that cannot upscale and renders at the
            // output size anyway is one whose frame times mean something else entirely.
            throw Error(
                "upscaling was asked for and this build has no DLSS; configure with "
                "-DOPENMW_RTX_DLSS=ON");
#endif
        }

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
        // Every frame in flight, and the presenter's last blit, before anything they name goes.
        mDevice.waitIdle();

        // Before the scenes below it, which own the storage the buried rooms are rooms in.
        emptyGraveyards();

        for (Frame& frame : mFrames)
        {
            vkDestroyFence(mDevice.getHandle(), frame.mFence, nullptr);
            vkDestroyFence(mDevice.getHandle(), frame.mGuiFence, nullptr);
        }
    }

    void VulkanRenderer::createTargets(std::uint32_t width, std::uint32_t height)
    {
        assert(width > 0 && height > 0);

        mOutputWidth = width;
        mOutputHeight = height;

        // Whatever upscales picks the render size; without one the two extents are the same number
        // twice, and every pass below is written as though they always might not be.
        VkExtent2D render{ width, height };
#ifdef OPENMW_RTX_DLSS
        if (mNgx != nullptr)
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
        mAccumulate.resize(mRenderWidth, mRenderHeight);
        mFilter.resize(mRenderWidth, mRenderHeight);

#ifdef OPENMW_RTX_DLSS
        // Released before the next is built: the feature holds the network's weights for one pair
        // of resolutions, which is most of what it occupies.
        mUpscaler.reset();

        if (mNgx != nullptr)
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
        std::uint32_t shownWidth = mRenderWidth;
        std::uint32_t shownHeight = mRenderHeight;
#ifdef OPENMW_RTX_DLSS
        if (mUpscaler != nullptr)
        {
            shownWidth = mOutputWidth;
            shownHeight = mOutputHeight;
        }
#endif
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

    VulkanRenderer::ViewScene& VulkanRenderer::sceneAt(std::uint32_t slot)
    {
        if (slot == sWorld)
            return mWorld;

        assert(slot < mViewScenes.size() && mViewScenes[slot] != nullptr && "a scene slot nothing holds");
        return *mViewScenes[slot];
    }

    const VulkanRenderer::ViewScene& VulkanRenderer::sceneAt(std::uint32_t slot) const
    {
        return const_cast<VulkanRenderer*>(this)->sceneAt(slot);
    }

    void VulkanRenderer::setScene(
        std::uint32_t slot, const SceneDesc& scene, std::span<const TextureData> textures, const SeaState& sea)
    {
        ViewScene& held = sceneAt(slot);

        // **Nothing may be in flight over what is about to go.** A rebuild is a load, and a load
        // waits: for the frames tracing the old scene, and for a placement the frame being recorded
        // may have submitted without a fence of its own.
        mDevice.waitIdle();
        finishFrames();
        emptyGraveyards();

        // Torn down before anything is built, so a second scene does not hold two of everything at
        // once — a cell's structures and textures are most of what this renderer occupies. The pass
        // is not among them; see below.
        held.mTextures.reset();
        held.mBuffers.reset();
        held.mAcceleration.reset();

        if (slot == sWorld)
        {
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
        Graveyard& graveyard = frameSlot(mFrame).mGraveyard;
        const std::uint32_t slots = slot == sWorld ? sFrameSlots : 1;

        held.mAcceleration
            = std::make_unique<SceneAcceleration>(mDevice, setup, scene, held.mRecords, textures, slots, graveyard);
        held.mBuffers = std::make_unique<SceneBuffers>(mDevice, scene, held.mRecords, slots, graveyard);
        held.mTextures = std::make_unique<TextureArray>(
            mDevice, setup, static_cast<std::uint32_t>(scene.getTextures().size()), textures, graveyard);
        held.mBuiltMeshes = scene.getMeshRevision();

        // **Built once and kept, because building one compiles the shader — half a second a time,
        // measured.** Nothing about the pass depends on the scene: it needs the device and the shape
        // of the texture set, and every array declares that shape identically — the bindless binding
        // is sized to its maximum rather than to the cell, so what varies between scenes is how many
        // descriptors get allocated and never what the layout says. Identically defined layouts are
        // compatible, so a set from a later array binds against the pipeline layout the first one
        // produced. `TextureArray`'s layout is where that invariant is kept.
        //
        // A doll can be the first thing this renderer ever builds — a race preview stands in front
        // of a game that has no world yet — and the pass belongs to neither scene.
        if (mPass == nullptr)
        {
            mPass = std::make_unique<VisibilityPass>(
                mDevice, setup, mShaderDirectory, held.mTextures->getLayout(), mChannelLayout, mCountHits);
            mTone = std::make_unique<TonePass>(mDevice, mPool, held.mTextures->getLayout(), mShaderDirectory);
        }

        // By hand rather than left to the destructor, so a submit that fails throws out of here
        // instead of being logged on the way past.
        setup.flush();

        if (slot == sWorld)
            mStats = SceneStats{
                .mInstances = held.mAcceleration->getInstanceCount(),
                .mCutoutInstances = held.mAcceleration->getCutoutInstanceCount(),
                .mMicromappedInstances = held.mAcceleration->getMicromappedInstanceCount(),
                .mMicromapTally = held.mAcceleration->getMicromapTally(),
                .mStructureBytes = held.mAcceleration->getStructureBytes(),
                .mTableBytes = held.mBuffers->getBytes(),
                .mTextureCount = held.mTextures->getCount(),
                .mTextureBytes = held.mTextures->getBytes(),
            };
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
        if (slot == sWorld)
            finishFrames();

        Graveyard& graveyard = frameSlot(mFrame).mGraveyard;

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
            held.mAcceleration->extend(setup, scene, arrived, graveyard);
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
            mStats = SceneStats{
                .mInstances = held.mAcceleration->getInstanceCount(),
                .mCutoutInstances = held.mAcceleration->getCutoutInstanceCount(),
                .mMicromappedInstances = held.mAcceleration->getMicromappedInstanceCount(),
                .mMicromapTally = held.mAcceleration->getMicromapTally(),
                .mStructureBytes = held.mAcceleration->getStructureBytes(),
                .mTableBytes = held.mBuffers->getBytes(),
                .mTextureCount = held.mTextures->getCount(),
                .mTextureBytes = held.mTextures->getBytes(),
            };
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

        held.mTextures->drop(textures, frameSlot(mFrame).mGraveyard);
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
            Graveyard& graveyard = frameSlot(mFrame).mGraveyard;
            held.mAcceleration->release(scene.getFreedMeshes(), graveyard);
            updateInstanceRecords(scene, held.mRecords, held.mChangedRecords);

            mPool.submitAndWait([&](VkCommandBuffer commands) {
                held.mAcceleration->place(commands, scene, held.mRecords, held.mChangedRecords, 0, nullptr, graveyard);
            });
            held.mBuffers->place(scene, held.mRecords, held.mChangedRecords, 0, graveyard);
            return;
        }

        // **A placement opens the frame, and a second placement before a trace closes it.** The
        // frame's report starts here and not at the trace: placing the world is the refit and the
        // top level, and a report that began at `renderFrame` would leave them out. A frame that
        // was placed and never traced still has to reach the queue with a fence, or its slot never
        // comes round again.
        if (frameSlot(mFrame).mPlaced)
            closeFrame();

        Frame& frame = beginFrame();
        frame.mPlaced = true;

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
            finishThrough(mReadBy[into]);

        // **What the scene let go of, given back here.** Walking away from a ring frees its meshes
        // and nothing arrives to take them over until the walk reaches the far side of the next one,
        // so a frame that only places is the one that must not hold their structures. Already done
        // where `extendScene` came through, and asking twice costs two comparisons a slot.
        held.mAcceleration->release(scene.getFreedMeshes(), frame.mGraveyard);

        // **Once, for the slots that changed, and both halves read it.** The rows carry a matrix
        // inverse apiece and a nine-by-nine exterior is fifty thousand of them; the acceleration
        // structure and the instance table were each building the whole set for themselves, every
        // frame, to change a hundred of them.
        updateInstanceRecords(scene, held.mRecords, held.mChangedRecords);

        // **The placement's own submit, without a fence and without a wait.** A picture inside the
        // interface traced before this frame's trace needs the top level to have reached the queue;
        // the frame's fence, later on the queue, covers this submit too. Nothing recorded is
        // nothing submitted, which is every frame of a standing camera in an empty place.
        mPool.begin(frame.mPlaceCommands);

        const bool built = held.mAcceleration->place(
            frame.mPlaceCommands, scene, held.mRecords, held.mChangedRecords, into, &frame.mTimer, frame.mGraveyard);
        if (built)
            mPool.submit(frame.mPlaceCommands, VK_NULL_HANDLE, frame.mGraveyard);
        else
            checkVk(vkEndCommandBuffer(frame.mPlaceCommands), "vkEndCommandBuffer");

        // **Only what a moving world changed**, which is the instance rows, the lights and the
        // vertices of anything skinned. Rebuilding all of it was measured at twenty to twenty-seven
        // milliseconds on a nine-by-nine region and was the largest single cost in the frame.
        held.mBuffers->place(scene, held.mRecords, held.mChangedRecords, into, frame.mGraveyard);

        mWorldSlot = into;

        mStats.mInstances = held.mAcceleration->getInstanceCount();
        mStats.mCutoutInstances = held.mAcceleration->getCutoutInstanceCount();
        mStats.mMicromappedInstances = held.mAcceleration->getMicromappedInstanceCount();
        mStats.mMicromapTally = held.mAcceleration->getMicromapTally();
        mStats.mTableBytes = held.mBuffers->getBytes();
    }

    VulkanRenderer::Frame& VulkanRenderer::beginFrame()
    {
        Frame& frame = frameSlot(mFrame);
        if (frame.mBegun)
            return frame;

        // **The frame that last used this slot has to be out of the way** — its fence waited, its
        // graveyard emptied, its results read or dropped — which is what caps the frames in flight
        // at the number of slots.
        while (mFrame - mFinished >= sFrameSlots)
            finishOldest();

        frame.mTimer.beginFrame();
        frame.mBegun = true;
        frame.mPlaced = false;
        frame.mReconstruction = Reconstruction{};
        return frame;
    }

    void VulkanRenderer::closeFrame()
    {
        Frame& frame = frameSlot(mFrame);
        assert(frame.mBegun && "a frame closed that was never begun");

        // A frame that traced nothing hit nothing; without this the count read back would be
        // whatever the slot's last frame left.
        if (mCountHits)
        {
            *static_cast<std::uint32_t*>(frame.mHitCount.map()) = 0;
            frame.mHitCount.unmap();
        }

        // Nothing to record: the fence is the point, so that the placement submitted before it is
        // covered and its slot comes round again.
        mPool.begin(frame.mCommands);
        submitFrame(frame);
    }

    void VulkanRenderer::submitFrame(Frame& frame)
    {
        mPool.submit(frame.mCommands, frame.mFence, frame.mGraveyard);

        frame.mBegun = false;
        frame.mPlaced = false;
        frame.mPending = true;
        ++mFrame;
    }

    FrameResult VulkanRenderer::finishOldest()
    {
        assert(mFinished < mFrame && "nothing in flight to finish");

        Frame& frame = frameSlot(mFinished);
        assert(frame.mPending && "a frame in flight that was never submitted");

        const auto start = std::chrono::steady_clock::now();
        awaitVk(mDevice.getHandle(), frame.mFence, "a frame");
        const double waited
            = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

        frame.mPending = false;

        // Read after the fence and never before: the count is the device's sum, and the queries
        // are the device's clock.
        std::uint32_t hits = 0;
        if (mCountHits)
        {
            hits = *static_cast<const std::uint32_t*>(frame.mHitCount.map());
            frame.mHitCount.unmap();
        }

        // What this frame may still have been reading is nothing's now.
        frame.mGraveyard.clear();

        ++mFinished;
        return FrameResult{
            .mHits = hits,
            .mWaitMs = waited,
            .mGpu = frame.mTimer.resolve(),
            .mReconstruction = frame.mReconstruction,
        };
    }

    std::optional<FrameResult> VulkanRenderer::finishFrame()
    {
        if (mFinished == mFrame)
            return std::nullopt;

        return finishOldest();
    }

    void VulkanRenderer::finishThrough(const std::uint64_t frame)
    {
        while (mFinished < mFrame && mFinished <= frame)
            finishOldest();
    }

    void VulkanRenderer::finishFrames()
    {
        while (mFinished < mFrame)
            finishOldest();
    }

    void VulkanRenderer::emptyGraveyards()
    {
        for (Frame& frame : mFrames)
        {
            frame.mGraveyard.clear();
            frame.mGuiGraveyard.clear();
        }
    }

    void VulkanRenderer::resize(std::uint32_t width, std::uint32_t height)
    {
        if (mPresenter != nullptr)
        {
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
        finishFrames();
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
        Frame& gui = frameSlot(mGuiFrame);
        if (gui.mGuiPending)
        {
            awaitVk(mDevice.getHandle(), gui.mGuiFence, "the interface drawn two frames ago");
            gui.mGuiPending = false;
            gui.mGuiGraveyard.clear();
        }

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
        Frame& frame = beginFrame();

        // **How long since the last one, which a motion vector cannot say.** A vector carries a
        // distance; how fast that was depends on the time it took, and the upscaler tunes how hard
        // it denoises against exactly that.
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        const float sinceLastMs
            = mLastFrameAt.has_value() ? std::chrono::duration<float, std::milli>(now - *mLastFrameAt).count() : 0.0f;
        mLastFrameAt = now;

        // The count is an atomic sum over the frame, so it starts each one at nothing — and it is
        // not started at all where the trace was specialized to write nothing into it, which is the
        // other half of taking the counter out of the game: the atomic went with `COUNT_HITS`, and
        // this is the two mappings a frame it never reads was still paying for. Here and not where
        // the frame opened, because a picture inside the interface traced between the two adds to
        // whichever buffer it is handed.
        if (mCountHits)
        {
            *static_cast<std::uint32_t*>(frame.mHitCount.map()) = 0;
            frame.mHitCount.unmap();
        }

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

        // **The one subtraction of two world points, and it happens here.** Two camera positions a
        // step apart subtract exactly in a float; the same difference taken on the device, between
        // coordinates six figures long, would be rounding.
        sampled.mCameraMotion = camera.mOrigin - mPreviousCamera.mOrigin;
        sampled.mPreviousForward = mPreviousCamera.mCamera.mForward;
        sampled.mPreviousRight = mPreviousCamera.mCamera.mRight;
        sampled.mPreviousUp = mPreviousCamera.mCamera.mUp;

        // **The sprite tiles are screen space, so they belong to the frame and not to the scene.**
        // Written before the recording below, which is what makes them visible to it without a
        // barrier — the same thing that lets the instance rows be written where the builder reads.
        // Into the copy this frame traces, which the frame before last is done with.
        mWorld.mBuffers->binSprites(camera.mOrigin, camera.mCamera, camera.mSunPosition, mWorldSlot, frame.mGraveyard);

        const VisibilityInputs inputs{
            .mScene = mWorld.mAcceleration->getTopLevel(),
            .mBuffers = mWorld.mBuffers.get(),
            .mSlot = mWorldSlot,
            .mIndexBlocks = mWorld.mAcceleration->getIndexBlocks(),
            .mTextures = mWorld.mTextures->getSet(),
            .mTextureLayout = mWorld.mTextures->getLayout(),
            .mShading = mWorld.mTextures->getShading(),
            .mWaves = &mWaves,
            .mFog = &mFog,
            .mWater = mWorld.mAcceleration->getWaterInstanceCount() > 0,
        };

        // Made by the first frame that averages, and that frame is the one that fills it.
        const bool fresh = options.mAccumulate > 0 && mSum == nullptr;
        if (fresh)
            mSum = std::make_unique<Image>(
                mDevice, mRenderWidth, mRenderHeight, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, "sum");

        // **A history is worthless after a jump no motion vector can describe.** A zero basis catches
        // the frames that have no past at all — a resize, a rebuild, the first one — and nothing
        // caught the rest: walking through a door left the previous camera intact and a reprojection
        // fetched one room onto another. `mHistoryStale` is the other half, and it is the signal the
        // renderer was already being sent. Both denoisers ask it, so it is answered once.
        const bool historyLost = mHistoryStale || mPreviousCamera.mCamera.mForward.length2() <= 0.0f;

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
        if (mUpscaled != nullptr)
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

        mChannels->begin(commands);
        timer.open(commands, "trace");
        mPass->record(commands, inputs, *mChannels, frame.mHitCount, sampled);
        timer.close(commands);
        mChannels->handOver(commands);

        // Where the bounce ended up: the filter's last level, or the channel the trace wrote
        // when nothing filtered it. **Ray Reconstruction is itself the denoiser**, and handing
        // it a frame the wavelet already blurred is asking it to recover what was thrown away —
        // which is why `resolve` never answers with both.
        const bool filtering = reconstruction.mDenoiser == Denoiser::Wavelet;
        const Image* indirect = &mChannels->getIndirect();
        if (filtering)
        {
            // **The temporal half first, and the cascade is what fills in where it was
            // rejected.** The accumulator replaces the trace's single sample with the mean of
            // the frames this surface has been seen over, and hands on the variance of that
            // mean — which is what lets the levels below stop at an edge in the light rather
            // than only at an edge in the geometry.
            timer.open(commands, "accumulate");
            const Image& moments = mAccumulate.record(commands, *mChannels, sampled.mCamera, historyLost);
            historyAnswered = true;
            timer.close(commands);

            // The cascade reads what the accumulator just wrote, in both channels.
            for (const Image* written : { &mChannels->getIndirect(), &moments })
                written->transition(commands, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

            timer.open(commands, "filter");
            indirect = &mFilter.record(commands, *mChannels, moments, sampled.mCamera);
            timer.close(commands);
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
        if (mUpscaler != nullptr)
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
        mReadBy[mWorldSlot] = mFrame;
        submitFrame(frame);

        // What the next frame reprojects against, and the camera as the caller gave it: a jitter is
        // where inside a pixel this frame sampled, not where the eye was.
        mPreviousCamera = camera;

        if (historyAnswered)
            mHistoryStale = false;

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
        finishFrames();
        emptyGraveyards();

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
        mViewAccumulate.resize(mViewWidth, mViewHeight);
        mViewFilter.resize(mViewWidth, mViewHeight);
    }

    void VulkanRenderer::traceGuiTexture(
        std::uint32_t texture, const Shaders::VisibilityConstants& camera, const GuiTraceOptions& options)
    {
        assert(mPass != nullptr && "traceGuiTexture before any scene was built");
        assert(camera.mCamera.mWidth == options.mWidth && camera.mCamera.mHeight == options.mHeight
            && "the camera has to be built for the part of the texture it fills");

        const bool ofTheWorld = options.mScene == sWorld;
        assert((ofTheWorld || (options.mScene < mViewScenes.size() && mViewScenes[options.mScene] != nullptr))
            && "a trace against a scene nothing holds");

        const bool held = mGuiTextures.holds(texture);
        assert(held && "a trace into a slot nothing holds");

        if (!held || options.mWidth == 0 || options.mHeight == 0)
            return;

        growViewTargets(options.mWidth, options.mHeight);

        // **Every frame in flight first.** A picture of the world binds the world's tables and
        // bins its sprites into them, and the frame that last traced them may still be reading;
        // a picture of a subject has tables of its own, but the pass, the sea and the fog below
        // are the frame's neighbours. A stall here is a picture's cost and not a frame's.
        finishFrames();

        const SceneAcceleration& acceleration
            = ofTheWorld ? *mWorld.mAcceleration : *mViewScenes[options.mScene]->mAcceleration;
        SceneBuffers* buffers = ofTheWorld ? mWorld.mBuffers.get() : mViewScenes[options.mScene]->mBuffers.get();
        const TextureArray& array = ofTheWorld ? *mWorld.mTextures : *mViewScenes[options.mScene]->mTextures;

        // A doll and a map trace the same shader, so they need the same list — against their own
        // camera, which is not the frame's.
        const std::uint32_t slot = ofTheWorld ? mWorldSlot : 0;
        buffers->binSprites(camera.mOrigin, camera.mCamera, camera.mSunPosition, slot, frameSlot(mFrame).mGraveyard);

        const VisibilityInputs inputs{
            .mScene = acceleration.getTopLevel(),
            .mBuffers = buffers,
            .mSlot = slot,
            .mIndexBlocks = acceleration.getIndexBlocks(),
            .mTextures = array.getSet(),
            .mTextureLayout = array.getLayout(),
            .mShading = array.getShading(),
            .mWaves = &mWaves,
            .mFog = &mFog,
            .mWater = acceleration.getWaterInstanceCount() > 0,
        };

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

            mViewChannels->begin(commands);
            mPass->record(commands, inputs, *mViewChannels, frameSlot(mFrame).mHitCount, camera);
            mViewChannels->handOver(commands);

            // A doll and a map tile are one frame with no frame before them, so the accumulator is
            // a pass-through that says so: no history, and the largest variance there is, which is
            // what tells the cascade to filter as widely as it can.
            const Image& viewMoments = mViewAccumulate.record(commands, *mViewChannels, camera.mCamera, true);
            const Image& indirect = mViewFilter.record(commands, *mViewChannels, viewMoments, camera.mCamera);

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
                array.getSet(), *mViewTarget,
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
            case Channel::BiasMask:
                image = &mChannels->getBiasMask();
                break;
            case Channel::Indirect:
                image = &mChannels->getIndirect();
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
        // **Where this backend's exceptions stop.** Everything below throws `Error` on a machine that
        // cannot run it — no loader, no driver, a device that does not qualify — and every caller
        // wants that as an answer rather than as an unwind.
        try
        {
            return std::make_unique<VulkanRenderer>(options);
        }
        catch (const Error& error)
        {
            reason = error.what();
            return nullptr;
        }
    }
}
