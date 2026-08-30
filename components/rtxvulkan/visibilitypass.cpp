#include "visibilitypass.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <exception>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include <components/rtx/bluenoise.hpp>

#include "buffer.hpp"
#include "commands.hpp"
#include "fogtile.hpp"
#include "fogvolume.hpp"
#include "gbuffer.hpp"
#include "gputimer.hpp"
#include "scenebuffers.hpp"
#include "wavepass.hpp"

namespace Rtx
{
    namespace
    {
        std::uint32_t groupsFor(std::uint32_t extent, std::uint32_t workgroup)
        {
            return (extent + workgroup - 1) / workgroup;
        }

        constexpr auto sCompute = VK_SHADER_STAGE_COMPUTE_BIT;
        constexpr auto sStorage = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

        /// Where the tables end and the frame's own block begins.
        constexpr std::uint32_t sFrameBinding = 20;

        /// The structure, the tables a hit reads, the frame itself and the sea, in the order the
        /// shader declares them. The channels the trace writes are not here: `GBuffer` says
        /// why they have a set of their own.
        constexpr std::array<VkDescriptorSetLayoutBinding, sFrameBinding + 4> sBindings = [] {
            std::array<VkDescriptorSetLayoutBinding, sFrameBinding + 4> declared{};
            declared[0] = VkDescriptorSetLayoutBinding{ 0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, sCompute };

            // One up to the frame are storage buffers: the hit count, then every table in the order
            // `record` writes them.
            for (std::uint32_t binding = 1; binding < sFrameBinding; ++binding)
                declared[binding] = VkDescriptorSetLayoutBinding{ binding, sStorage, 1, sCompute };

            declared[sFrameBinding]
                = VkDescriptorSetLayoutBinding{ sFrameBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, sCompute };

            // Then the two the sea was synthesised into, one descriptor a cascade.
            for (std::uint32_t binding = sFrameBinding + 1; binding < sFrameBinding + 3; ++binding)
                declared[binding] = VkDescriptorSetLayoutBinding{ binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    Shaders::WAVE_CASCADES, sCompute };

            // And the one the fog's field was drawn into, which is one volume rather than a cascade
            // of tiles: the air has no near band and no far one, it has a field read at three scales.
            declared[sFrameBinding + 3] = VkDescriptorSetLayoutBinding{ sFrameBinding + 3,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, sCompute };

            return declared;
        }();
    }

    VisibilityVariant VisibilityVariant::resolve(const Shaders::VisibilityConstants& frame, const bool water)
    {
        // **A moon that is drawn and a moon that lights are two facts**, and the sky needs the first
        // where no surface asks for the second: the game fades both out over the hours around dawn,
        // and a disc still on its way down lights nothing.
        bool moons = false;
        for (const Shaders::MoonDisc& moon : frame.mMoons)
            moons = moons || moon.mAlpha > 0.0f || moon.mIrradiance != Shaders::vec3();

        return VisibilityVariant{
            // Nought exactly where the sun is not up, and it fades to that across dusk rather than
            // stepping — `VisibilityConstants::mSunIrradiance` says why there is no second field.
            .mSun = frame.mSunIrradiance != Shaders::vec3(),
            .mMoons = moons,

            // Either half is water in the frame: a surface the eye can meet, or a level it can be
            // under. A cell with a level and no surface is one the eye can still be submerged in.
            .mSea = water || !std::isinf(frame.mWaterLevel),
            .mUniformFog = frame.mFogUniform >= 1.0f,
        };
    }

    std::uint32_t VisibilityVariant::index() const
    {
        return (mSun ? 1u : 0u) | (mMoons ? 2u : 0u) | (mSea ? 4u : 0u) | (mUniformFog ? 8u : 0u);
    }

    std::string VisibilityVariant::describe(const std::string_view kernel) const
    {
        std::string name(kernel);
        if (mSun)
            name += " sun";
        if (mMoons)
            name += " moons";
        if (mSea)
            name += " sea";
        if (mUniformFog)
            name += " even-air";
        return name;
    }

    VisibilityPass::VisibilityPass(const Device& device, Batch& batch, const std::filesystem::path& shaderDirectory,
        VkDescriptorSetLayout textureLayout, const SetLayout& channelLayout, const SetLayout& volumeLayout,
        bool countHits)
        : mDevice(device)
        , mBlueNoise(uploadBuffer(device, batch, BlueNoise::shared().getValues(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))
        , mConstants(device, sizeof(Shaders::VisibilityConstants),
              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        , mCountHits(countHits ? 1u : 0u)
        , mChannelLayout(channelLayout.getHandle())
        , mVolumeLayout(volumeLayout.getHandle())
        , mModule(shaderDirectory / "visibility.comp.spv")
        , mVolumeModule(shaderDirectory / "fogvolume.comp.spv")
    {
        compileEvery(textureLayout);
    }

    void VisibilityPass::compileEvery(VkDescriptorSetLayout textureLayout)
    {
        /// One kernel to make: which tuple, and which of the two modules.
        struct Wanted
        {
            VisibilityVariant mVariant;
            bool mVolume = false;
        };

        std::vector<Wanted> wanted;
        wanted.reserve(VisibilityVariant::sCount + VisibilityVariant::sCount / 2);

        for (const bool sun : { false, true })
            for (const bool moons : { false, true })
                for (const bool sea : { false, true })
                    for (const bool evenAir : { false, true })
                    {
                        const VisibilityVariant variant{
                            .mSun = sun, .mMoons = moons, .mSea = sea, .mUniformFog = evenAir
                        };
                        wanted.push_back(Wanted{ .mVariant = variant });

                        // A room reads the closed form, so it needs no volume and gets no kernel.
                        if (!evenAir)
                            wanted.push_back(Wanted{ .mVariant = variant, .mVolume = true });
                    }

        std::atomic<std::size_t> next{ 0 };
        std::mutex kept;
        std::exception_ptr failed;

        const auto compile = [&] {
            for (std::size_t at = next++; at < wanted.size(); at = next++)
            {
                try
                {
                    const VisibilityVariant variant = wanted[at].mVariant;
                    const bool volume = wanted[at].mVolume;

                    // One word per `constant_id`, in the order `lib/variants.glsl` declares them.
                    // The volume traces no primary ray, so it counts none whatever the build asked
                    // for; every other constant it takes is the tuple's own, and no even air ever
                    // reaches the volume's table.
                    const std::array<std::uint32_t, 5> specialization{ volume ? 0u : mCountHits, variant.mSun ? 1u : 0u,
                        variant.mMoons ? 1u : 0u, variant.mSea ? 1u : 0u, variant.mUniformFog ? 1u : 0u };

                    auto& table = volume ? mVolumePipelines : mPipelines;
                    table[variant.index()] = std::make_unique<ComputePipeline>(mDevice, sBindings, 0,
                        laterSets(textureLayout), volume ? mVolumeModule : mModule,
                        variant.describe(volume ? "fog volume" : "visibility"), specialization);
                }
                catch (...)
                {
                    // **The first failure and not the last**, so the message names what actually
                    // went wrong rather than whichever thread finished after it.
                    const std::lock_guard<std::mutex> hold(kept);
                    if (failed == nullptr)
                        failed = std::current_exception();
                }
            }
        };

        const std::size_t hands
            = std::max<std::size_t>(1, std::min<std::size_t>(std::thread::hardware_concurrency(), wanted.size()));

        {
            std::vector<std::jthread> compiling;
            compiling.reserve(hands);
            for (std::size_t hand = 0; hand < hands; ++hand)
                compiling.emplace_back(compile);
        }

        if (failed != nullptr)
            std::rethrow_exception(failed);
    }

    const ComputePipeline& VisibilityPass::pipelineFor(const VisibilityVariant variant) const
    {
        const std::unique_ptr<ComputePipeline>& held = mPipelines[variant.index()];
        assert(held != nullptr && "a tuple `compileEvery` did not make");

        return *held;
    }

    const ComputePipeline* VisibilityPass::volumePipelineFor(const VisibilityVariant variant) const
    {
        assert((variant.mUniformFog || mVolumePipelines[variant.index()] != nullptr)
            && "a banked air with no volume kernel made for it");

        return mVolumePipelines[variant.index()].get();
    }

    std::array<VkDescriptorSetLayout, 3> VisibilityPass::laterSets(VkDescriptorSetLayout textureLayout) const
    {
        return { textureLayout, mChannelLayout, mVolumeLayout };
    }

    void VisibilityPass::writeConstants(VkCommandBuffer commands, const Shaders::VisibilityConstants& described) const
    {
        // **Both directions, because one buffer serves every trace.** The write has to wait for the
        // last dispatch that read it — a traced view and the world are two traces — and the next
        // dispatch has to wait for the write. **And for the last write**, which two frames in
        // flight make the frame before's own update of this buffer: a write after a write is a
        // hazard of its own, and the dispatch between them is not what orders it.
        const VkBufferMemoryBarrier2 beforeWrite{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT,
            .srcAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = mConstants.getHandle(),
            .size = VK_WHOLE_SIZE,
        };
        const VkDependencyInfo settle{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &beforeWrite,
        };
        vkCmdPipelineBarrier2(commands, &settle);

        // A few hundred bytes, so this is an inline write into the command buffer rather than a
        // staging copy — and being recorded, it runs in queue order with the traces around it. A
        // clear command and not a copy, which is what the stage on either side of it says: the
        // specification files `vkCmdUpdateBuffer` under the clear commands, and a barrier at the
        // copy stage leaves the write outside its scope.
        vkCmdUpdateBuffer(commands, mConstants.getHandle(), 0, sizeof(described), &described);

        const VkBufferMemoryBarrier2 written{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = mConstants.getHandle(),
            .size = VK_WHOLE_SIZE,
        };
        const VkDependencyInfo handOver{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &written,
        };
        vkCmdPipelineBarrier2(commands, &handOver);
    }

    void VisibilityPass::pushInputs(VkCommandBuffer commands, const ComputePipeline& pipeline,
        const VisibilityInputs& inputs, const GBuffer& buffer, const Buffer& hitCount, std::uint64_t frame) const
    {
        const VkWriteDescriptorSetAccelerationStructureKHR sceneWrite{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
            .accelerationStructureCount = 1,
            .pAccelerationStructures = &inputs.mScene,
        };
        // Bindings one upwards are all storage buffers, in the order the shader declares them.
        const std::array<VkDescriptorBufferInfo, 12> buffers{
            VkDescriptorBufferInfo{ hitCount.getHandle(), 0, VK_WHOLE_SIZE },
            VkDescriptorBufferInfo{ inputs.mBuffers->getNormalBlocks(inputs.mSlot), 0, VK_WHOLE_SIZE },
            VkDescriptorBufferInfo{ inputs.mBuffers->getTexCoordBlocks(), 0, VK_WHOLE_SIZE },
            VkDescriptorBufferInfo{ inputs.mIndexBlocks, 0, VK_WHOLE_SIZE },
            VkDescriptorBufferInfo{ inputs.mBuffers->getMeshes(), 0, VK_WHOLE_SIZE },
            VkDescriptorBufferInfo{ inputs.mBuffers->getInstances(inputs.mSlot), 0, VK_WHOLE_SIZE },
            VkDescriptorBufferInfo{ inputs.mBuffers->getMaterials(inputs.mSlot), 0, VK_WHOLE_SIZE },
            VkDescriptorBufferInfo{ inputs.mBuffers->getLayers(inputs.mSlot), 0, VK_WHOLE_SIZE },
            VkDescriptorBufferInfo{ inputs.mBuffers->getMasks(inputs.mSlot), 0, VK_WHOLE_SIZE },
            VkDescriptorBufferInfo{ inputs.mBuffers->getLights(inputs.mSlot), 0, VK_WHOLE_SIZE },
            VkDescriptorBufferInfo{ inputs.mBuffers->getLightOffsets(inputs.mSlot), 0, VK_WHOLE_SIZE },
            VkDescriptorBufferInfo{ inputs.mBuffers->getLightIndices(inputs.mSlot), 0, VK_WHOLE_SIZE },
        };
        const VkDescriptorBufferInfo noiseWrite{ mBlueNoise.getHandle(), 0, VK_WHOLE_SIZE };
        const VkDescriptorBufferInfo shadingWrite{ inputs.mShading, 0, VK_WHOLE_SIZE };
        const VkDescriptorBufferInfo gridWrite{ inputs.mBuffers->getGrid(inputs.mSlot), 0, VK_WHOLE_SIZE };
        const VkDescriptorBufferInfo spriteWrite{ inputs.mBuffers->getSprites(inputs.mSlot), 0, VK_WHOLE_SIZE };
        const VkDescriptorBufferInfo emitterWrite{ inputs.mBuffers->getEmitters(inputs.mSlot), 0, VK_WHOLE_SIZE };
        const VkDescriptorBufferInfo tileOffsetWrite{ inputs.mBuffers->getSpriteTileOffsets(inputs.mSlot), 0,
            VK_WHOLE_SIZE };
        const VkDescriptorBufferInfo tileIndexWrite{ inputs.mBuffers->getSpriteTileIndices(inputs.mSlot), 0,
            VK_WHOLE_SIZE };
        const VkDescriptorBufferInfo frameWrite{ mConstants.getHandle(), 0, VK_WHOLE_SIZE };

        // **The tiles' widths come off the pass that built them**, so what the shader divides by is
        // what is actually bound rather than a second statement of the same table.
        std::array<VkDescriptorImageInfo, Shaders::WAVE_CASCADES> surfaces{};
        std::array<VkDescriptorImageInfo, Shaders::WAVE_CASCADES> curvatures{};
        for (std::size_t cascade = 0; cascade < Shaders::WAVE_CASCADES; ++cascade)
        {
            const VkSampler sampler = inputs.mWaves->getSampler();
            surfaces[cascade] = { sampler, inputs.mWaves->getSurface(cascade).getView(), VK_IMAGE_LAYOUT_GENERAL };
            curvatures[cascade] = { sampler, inputs.mWaves->getCurvature(cascade).getView(), VK_IMAGE_LAYOUT_GENERAL };
        }

        // **Nothing bound here may be nothing.** A descriptor the shader declares and a null handle
        // is undefined at the dispatch: the driver may fault, may not, and says nothing either way —
        // it cost this renderer a device and five seconds of a wedged process before the layers were
        // asked. Every one of these is a table an owner promises to have opened or an input a caller
        // promises to pass, so a null is a broken promise and not a state to handle.
        [[maybe_unused]] const auto bound
            = [](const VkDescriptorBufferInfo& write) { return write.buffer != VK_NULL_HANDLE; };
        assert(std::all_of(buffers.begin(), buffers.end(), bound) && "a table bound as nothing");
        assert(bound(noiseWrite) && bound(shadingWrite) && bound(gridWrite) && bound(spriteWrite) && bound(emitterWrite)
            && bound(tileOffsetWrite) && bound(tileIndexWrite) && bound(frameWrite) && "an input bound as nothing");

        // **Appended rather than indexed.** Every one of these used to name its own slot — channels
        // at `1 + i`, buffers at `i + 8`, then twenty-one through twenty-six by hand — so adding a
        // channel silently moved two buffer writes on top of each other and left the new bindings
        // unwritten. The layout said what was wrong and nothing else did. A cursor cannot make that
        // mistake, and the count below is checked rather than maintained.
        std::array<VkWriteDescriptorSet, sBindings.size()> writes{};
        std::uint32_t filled = 0;

        const auto append
            = [&](std::uint32_t binding, VkDescriptorType type, const void* next, const VkDescriptorImageInfo* image,
                  const VkDescriptorBufferInfo* block, std::uint32_t count = 1) {
                  assert(filled < writes.size() && "more descriptor writes than the layout has bindings");
                  writes[filled++] = VkWriteDescriptorSet{
                      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                      .pNext = next,
                      .dstBinding = binding,
                      .descriptorCount = count,
                      .descriptorType = type,
                      .pImageInfo = image,
                      .pBufferInfo = block,
                  };
              };
        const auto appendBuffer = [&](std::uint32_t binding, const VkDescriptorBufferInfo& block) {
            append(binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, nullptr, &block);
        };
        const auto appendUniform = [&](std::uint32_t binding, const VkDescriptorBufferInfo& block) {
            append(binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, nullptr, &block);
        };
        const auto appendImages
            = [&](std::uint32_t binding, const std::array<VkDescriptorImageInfo, Shaders::WAVE_CASCADES>& images) {
                  append(binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, images.data(), nullptr,
                      Shaders::WAVE_CASCADES);
              };

        // The one write whose payload hangs off `pNext` rather than off a pointer field.
        append(0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, &sceneWrite, nullptr, nullptr);

        for (std::uint32_t i = 0; i < buffers.size(); ++i)
            appendBuffer(i + 1, buffers[i]);

        appendBuffer(13, noiseWrite);
        appendBuffer(14, shadingWrite);
        appendBuffer(15, gridWrite);
        appendBuffer(16, spriteWrite);
        appendBuffer(17, emitterWrite);
        appendBuffer(18, tileOffsetWrite);
        appendBuffer(19, tileIndexWrite);
        appendUniform(sFrameBinding, frameWrite);

        // **Sampled from `GENERAL` rather than moved to a read-only layout**, for the reason
        // `BloomPass` gives: these are written as storage images and read as sampled ones a few
        // dispatches apart, and `GENERAL` is the one layout both accesses are legal from.
        appendImages(sFrameBinding + 1, surfaces);
        appendImages(sFrameBinding + 2, curvatures);

        const VkDescriptorImageInfo fogWrite{ inputs.mFog->getSampler(), inputs.mFog->getField().getView(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        append(sFrameBinding + 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &fogWrite, nullptr);

        // Every binding the layout declares, written exactly once — a shader that grew one and a
        // record that did not is the failure this counts.
        assert(filled == writes.size() && "a binding the layout declares was left unwritten");

        vkCmdPushDescriptorSet(
            commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.getLayout(), 0, filled, writes.data());

        // The three sets nothing pushes: the bindless textures a scene brought, the channels the
        // trace writes, and the air in front of the camera. Each is written when what it names is
        // made, and bound as it is.
        const std::array<VkDescriptorSet, 3> sets{ inputs.mTextures, buffer.getSet(),
            inputs.mFogVolume->getSet(frame) };
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.getLayout(), 1,
            static_cast<std::uint32_t>(sets.size()), sets.data(), 0, nullptr);
    }

    void VisibilityPass::record(VkCommandBuffer commands, const VisibilityInputs& inputs, const GBuffer& buffer,
        const Buffer& hitCount, const Shaders::VisibilityConstants& constants, const bool historyLost,
        GpuTimer* timer) const
    {
        assert(buffer.getWidth() >= constants.mCamera.mWidth && buffer.getHeight() >= constants.mCamera.mHeight);

        assert(inputs.mWaves != nullptr && "a trace with no sea synthesised for it");
        assert(inputs.mFog != nullptr && "a trace with no fog field drawn for it");
        assert(inputs.mFogVolume != nullptr && "a trace with no air integrated for it");
        assert(inputs.mTextures != VK_NULL_HANDLE && "a trace whose texture array named no set");

        Shaders::VisibilityConstants described = constants;

        // **A basis of nothing is how this block already says there is no previous frame**, so a
        // door or a rebuild is told to every reprojection at once rather than to each of them
        // separately. The frame that carries it reprojects nothing, which is what it is for.
        if (historyLost)
        {
            described.mPreviousForward = Shaders::vec3();
            described.mPreviousRight = Shaders::vec3();
            described.mPreviousUp = Shaders::vec3();
        }

        // **The tiles' widths come off the pass that built them**, so what the shader divides by is
        // what is actually bound rather than a second statement of the same table.
        for (std::size_t cascade = 0; cascade < Shaders::WAVE_CASCADES; ++cascade)
            described.mWaveExtent[cascade] = inputs.mWaves->getExtent(cascade);

        described.mWaveSlope = inputs.mWaves->getSlope();

        const WaveCurvature& curvature = inputs.mWaves->getMoments();
        described.mWaveCurvature = curvature.mWhole;
        std::copy(curvature.mResolved.begin(), curvature.mResolved.end(), std::begin(described.mWaveResolved));

        writeConstants(commands, described);

        // **Resolved from the constants this frame is about to be traced with**, and from nothing
        // kept between frames: a dusk moves the tuple and a doorway moves it again.
        const VisibilityVariant variant = VisibilityVariant::resolve(constants, inputs.mWater);

        // Taken for writing whether or not it is filled, because the trace binds a descriptor to it
        // either way and a descriptor names the layout its image is in.
        inputs.mFogVolume->begin(commands, constants.mFrame);

        if (const ComputePipeline* air = volumePipelineFor(variant); air != nullptr)
        {
            openZone(timer, commands, "air");

            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, air->getHandle());
            pushInputs(commands, *air, inputs, buffer, hitCount, constants.mFrame);

            // **Every column the image has and not every column the camera needs.** A traced view is
            // drawn into a volume grown to the largest one asked for, and the pixel at its edge
            // interpolates against the column outside it — which has to hold air rather than
            // whatever was there.
            vkCmdDispatch(commands, groupsFor(inputs.mFogVolume->getColumns(), Shaders::FOG_VOLUME_WORKGROUP),
                groupsFor(inputs.mFogVolume->getRows(), Shaders::FOG_VOLUME_WORKGROUP), 1);

            closeZone(timer, commands);

            inputs.mFogVolume->handOver(commands);
        }

        openZone(timer, commands, "trace");

        const ComputePipeline& pipeline = pipelineFor(variant);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.getHandle());
        pushInputs(commands, pipeline, inputs, buffer, hitCount, constants.mFrame);
        vkCmdDispatch(commands, groupsFor(constants.mCamera.mWidth, Shaders::VISIBILITY_WORKGROUP),
            groupsFor(constants.mCamera.mHeight, Shaders::VISIBILITY_WORKGROUP), 1);

        closeZone(timer, commands);
    }
}
