#include "visibilitypass.hpp"

#include <algorithm>
#include <cassert>

#include <array>
#include <cassert>
#include <span>

#include <components/rtx/bluenoise.hpp>

#include "buffer.hpp"
#include "commands.hpp"
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
        constexpr std::array<VkDescriptorSetLayoutBinding, sFrameBinding + 3> sBindings = [] {
            std::array<VkDescriptorSetLayoutBinding, sFrameBinding + 3> declared{};
            declared[0] = VkDescriptorSetLayoutBinding{ 0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, sCompute };

            // One up to the frame are storage buffers: the hit count, then every table in the order
            // `record` writes them.
            for (std::uint32_t binding = 1; binding < sFrameBinding; ++binding)
                declared[binding] = VkDescriptorSetLayoutBinding{ binding, sStorage, 1, sCompute };

            declared[sFrameBinding]
                = VkDescriptorSetLayoutBinding{ sFrameBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, sCompute };

            // And the three the sea was synthesised into, one descriptor a cascade.
            for (std::uint32_t binding = sFrameBinding + 1; binding < declared.size(); ++binding)
                declared[binding] = VkDescriptorSetLayoutBinding{ binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    Shaders::WAVE_CASCADES, sCompute };

            return declared;
        }();
    }

    VisibilityPass::VisibilityPass(const Device& device, Batch& batch, const std::filesystem::path& shaderDirectory,
        VkDescriptorSetLayout textureLayout, const GBufferLayout& channelLayout, bool countHits)
        : mBlueNoise(uploadBuffer(device, batch, BlueNoise::shared().getValues(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))
        , mConstants(device, sizeof(Shaders::VisibilityConstants),
              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        , mCountHits(countHits ? 1u : 0u)
        , mLaterSets{ textureLayout, channelLayout.getHandle() }
        , mPipeline(device, sBindings, 0, mLaterSets, shaderDirectory / "visibility.comp.spv", "visibility",
              std::span(&mCountHits, 1))
    {
    }

    void VisibilityPass::record(VkCommandBuffer commands, const VisibilityInputs& inputs, const GBuffer& buffer,
        const Buffer& hitCount, const Shaders::VisibilityConstants& constants) const
    {
        assert(buffer.getWidth() >= constants.mCamera.mWidth && buffer.getHeight() >= constants.mCamera.mHeight);

        assert(inputs.mWaves != nullptr && "a trace with no sea synthesised for it");

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
        described.mWaveTravel = inputs.mWaves->getTravel();

        const WaveCurvature& curvature = inputs.mWaves->getMoments();
        described.mWaveCurvature = curvature.mWhole;
        std::copy(curvature.mResolved.begin(), curvature.mResolved.end(), std::begin(described.mWaveResolved));

        // **Both directions, because one buffer serves every trace.** The write has to wait for the
        // last dispatch that read it — a traced view and the world are two traces — and the next
        // dispatch has to wait for the write.
        const VkBufferMemoryBarrier2 beforeWrite{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
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
        // staging copy — and being recorded, it runs in queue order with the traces around it.
        vkCmdUpdateBuffer(commands, mConstants.getHandle(), 0, sizeof(described), &described);

        const VkBufferMemoryBarrier2 written{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
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
            VkDescriptorBufferInfo{ inputs.mBuffers->getNormalBlocks(), 0, VK_WHOLE_SIZE },
            VkDescriptorBufferInfo{ inputs.mBuffers->getTexCoordBlocks(), 0, VK_WHOLE_SIZE },
            VkDescriptorBufferInfo{ inputs.mIndexBlocks, 0, VK_WHOLE_SIZE },
            VkDescriptorBufferInfo{ inputs.mBuffers->getMeshes(), 0, VK_WHOLE_SIZE },
            VkDescriptorBufferInfo{ inputs.mBuffers->getInstances(), 0, VK_WHOLE_SIZE },
            VkDescriptorBufferInfo{ inputs.mBuffers->getMaterials(), 0, VK_WHOLE_SIZE },
            VkDescriptorBufferInfo{ inputs.mBuffers->getLayers(), 0, VK_WHOLE_SIZE },
            VkDescriptorBufferInfo{ inputs.mBuffers->getMasks(), 0, VK_WHOLE_SIZE },
            VkDescriptorBufferInfo{ inputs.mBuffers->getLights(), 0, VK_WHOLE_SIZE },
            VkDescriptorBufferInfo{ inputs.mBuffers->getLightOffsets(), 0, VK_WHOLE_SIZE },
            VkDescriptorBufferInfo{ inputs.mBuffers->getLightIndices(), 0, VK_WHOLE_SIZE },
        };
        const VkDescriptorBufferInfo noiseWrite{ mBlueNoise.getHandle(), 0, VK_WHOLE_SIZE };
        const VkDescriptorBufferInfo shadingWrite{ inputs.mShading, 0, VK_WHOLE_SIZE };
        const VkDescriptorBufferInfo gridWrite{ inputs.mBuffers->getGrid(), 0, VK_WHOLE_SIZE };
        const VkDescriptorBufferInfo spriteWrite{ inputs.mBuffers->getSprites(), 0, VK_WHOLE_SIZE };
        const VkDescriptorBufferInfo emitterWrite{ inputs.mBuffers->getEmitters(), 0, VK_WHOLE_SIZE };
        const VkDescriptorBufferInfo tileOffsetWrite{ inputs.mBuffers->getSpriteTileOffsets(), 0, VK_WHOLE_SIZE };
        const VkDescriptorBufferInfo tileIndexWrite{ inputs.mBuffers->getSpriteTileIndices(), 0, VK_WHOLE_SIZE };
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

        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mPipeline.getHandle());
        // Every binding the layout declares, written exactly once — a shader that grew one and a
        // record that did not is the failure this counts.
        assert(filled == writes.size() && "a binding the layout declares was left unwritten");

        vkCmdPushDescriptorSet(
            commands, VK_PIPELINE_BIND_POINT_COMPUTE, mPipeline.getLayout(), 0, filled, writes.data());

        // The two sets nothing pushes: the bindless textures a scene brought, and the channels this
        // writes. Both are written when what they name is made, and bound as they are.
        const std::array<VkDescriptorSet, 2> sets{ inputs.mTextures, buffer.getSet() };
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mPipeline.getLayout(), 1,
            static_cast<std::uint32_t>(sets.size()), sets.data(), 0, nullptr);
        vkCmdDispatch(commands, groupsFor(constants.mCamera.mWidth), groupsFor(constants.mCamera.mHeight), 1);
    }

}
