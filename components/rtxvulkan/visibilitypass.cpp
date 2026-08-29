#include "visibilitypass.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <span>

#include <components/rtx/bluenoise.hpp>

#include "buffer.hpp"
#include "commands.hpp"
#include "fogtile.hpp"
#include "gbuffer.hpp"
#include "scenebuffers.hpp"
#include "wavepass.hpp"

namespace Rtx
{
    namespace
    {
        std::uint32_t groupsFor(std::uint32_t extent)
        {
            return (extent + Shaders::VISIBILITY_WORKGROUP - 1) / Shaders::VISIBILITY_WORKGROUP;
        }

        constexpr auto sCompute = VK_SHADER_STAGE_COMPUTE_BIT;
        constexpr auto sStorage = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

        /// Where the tables end and the frame's own block begins.
        constexpr std::uint32_t sFrameBinding = 20;

        /// The structure, the tables a hit reads, the frame itself and the sea, in the order the
        /// shader declares them. The channels the trace writes are not here: `GBufferLayout` says
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

    std::string VisibilityVariant::describe() const
    {
        std::string name = "visibility";
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
        VkDescriptorSetLayout textureLayout, const GBufferLayout& channelLayout, bool countHits)
        : mDevice(device)
        , mBlueNoise(uploadBuffer(device, batch, BlueNoise::shared().getValues(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))
        , mConstants(device, sizeof(Shaders::VisibilityConstants),
              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        , mCountHits(countHits ? 1u : 0u)
        , mChannelLayout(channelLayout.getHandle())
        , mModule(shaderDirectory / "visibility.comp.spv")
    {
        // **The three a game actually spends its time in**, compiled here where a load already
        // stands still rather than on the frame that first asks for one. The day and the night are
        // the pair a dusk crosses with nothing loading, which is the one transition a hitch would
        // be seen at.
        for (const VisibilityVariant common : {
                 VisibilityVariant{ .mSun = true, .mMoons = false, .mSea = true, .mUniformFog = false },
                 VisibilityVariant{ .mSun = false, .mMoons = true, .mSea = true, .mUniformFog = false },
                 VisibilityVariant{ .mSun = false, .mMoons = false, .mSea = false, .mUniformFog = true },
             })
            pipelineFor(common, textureLayout);
    }

    ComputePipeline& VisibilityPass::pipelineFor(const VisibilityVariant variant, VkDescriptorSetLayout textureLayout)
    {
        std::unique_ptr<ComputePipeline>& held = mPipelines[variant.index()];
        if (held == nullptr)
        {
            // One word per `constant_id`, in the order `lib/variants.glsl` declares them.
            const std::array<std::uint32_t, 5> specialization{ mCountHits, variant.mSun ? 1u : 0u,
                variant.mMoons ? 1u : 0u, variant.mSea ? 1u : 0u, variant.mUniformFog ? 1u : 0u };

            const std::array<VkDescriptorSetLayout, 2> laterSets{ textureLayout, mChannelLayout };
            held = std::make_unique<ComputePipeline>(
                mDevice, sBindings, 0, laterSets, mModule, variant.describe(), specialization);
        }

        return *held;
    }

    void VisibilityPass::record(VkCommandBuffer commands, const VisibilityInputs& inputs, const GBuffer& buffer,
        const Buffer& hitCount, const Shaders::VisibilityConstants& constants)
    {
        assert(buffer.getWidth() >= constants.mCamera.mWidth && buffer.getHeight() >= constants.mCamera.mHeight);

        assert(inputs.mWaves != nullptr && "a trace with no sea synthesised for it");
        assert(inputs.mFog != nullptr && "a trace with no fog field drawn for it");
        assert(inputs.mTextureLayout != VK_NULL_HANDLE && "a trace whose texture array named no layout");

        // **How many emitters there are is the scene's answer and not the camera's.** The table
        // never shrinks, so its length says nothing about this frame; taking the count off the
        // buffers here is what keeps a caller from having to know the table exists at all.
        Shaders::VisibilityConstants described = constants;

        // **The tiles' widths come off the pass that built them**, so what the shader divides by is
        // what is actually bound rather than a second statement of the same table.
        std::array<VkDescriptorImageInfo, Shaders::WAVE_CASCADES> surfaces{};
        std::array<VkDescriptorImageInfo, Shaders::WAVE_CASCADES> curvatures{};

        for (std::size_t cascade = 0; cascade < Shaders::WAVE_CASCADES; ++cascade)
        {
            const VkSampler sampler = inputs.mWaves->getSampler();
            surfaces[cascade] = { sampler, inputs.mWaves->getSurface(cascade).getView(), VK_IMAGE_LAYOUT_GENERAL };
            curvatures[cascade] = { sampler, inputs.mWaves->getCurvature(cascade).getView(), VK_IMAGE_LAYOUT_GENERAL };

            described.mWaveExtent[cascade] = inputs.mWaves->getExtent(cascade);
        }

        described.mWaveSlope = inputs.mWaves->getSlope();

        const WaveCurvature& curvature = inputs.mWaves->getMoments();
        described.mWaveCurvature = curvature.mWhole;
        std::copy(curvature.mResolved.begin(), curvature.mResolved.end(), std::begin(described.mWaveResolved));

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

        // **Resolved from the constants this frame is about to be traced with**, and from nothing
        // kept between frames: a dusk moves the tuple and a doorway moves it again.
        const ComputePipeline& pipeline
            = pipelineFor(VisibilityVariant::resolve(constants, inputs.mWater), inputs.mTextureLayout);

        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.getHandle());
        // Every binding the layout declares, written exactly once — a shader that grew one and a
        // record that did not is the failure this counts.
        assert(filled == writes.size() && "a binding the layout declares was left unwritten");

        vkCmdPushDescriptorSet(
            commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.getLayout(), 0, filled, writes.data());

        // The two sets nothing pushes: the bindless textures a scene brought, and the channels this
        // writes. Both are written when what they name is made, and bound as they are.
        const std::array<VkDescriptorSet, 2> sets{ inputs.mTextures, buffer.getSet() };
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.getLayout(), 1,
            static_cast<std::uint32_t>(sets.size()), sets.data(), 0, nullptr);
        vkCmdDispatch(commands, groupsFor(constants.mCamera.mWidth), groupsFor(constants.mCamera.mHeight), 1);
    }

}
