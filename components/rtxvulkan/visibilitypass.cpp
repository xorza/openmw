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
        constexpr auto sImage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

        /// The structure, the channels it writes, the tables a hit reads and the frame itself, in the
        /// order the shader declares them. The channels are scattered through the numbering because
        /// each grew onto the end of a layout that already existed rather than renumbering the
        /// tables under them.
        constexpr std::array<VkDescriptorSetLayoutBinding, 32> sBindings{
            VkDescriptorSetLayoutBinding{ 0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 1, sImage, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 2, sStorage, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 3, sStorage, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 4, sStorage, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 5, sStorage, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 6, sStorage, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 7, sStorage, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 8, sStorage, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 9, sStorage, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 10, sStorage, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 11, sStorage, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 12, sStorage, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 13, sStorage, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 14, sStorage, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 15, sImage, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 16, sStorage, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 17, sImage, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 18, sImage, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 19, sStorage, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 20, sImage, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 21, sStorage, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 22, sImage, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 23, sStorage, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 24, sStorage, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 25, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 30, sStorage, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 31, sStorage, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 26, sImage, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 27, sImage, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 28, sImage, 1, sCompute },
            VkDescriptorSetLayoutBinding{ 29, sImage, 1, sCompute },
        };
    }

    VisibilityPass::VisibilityPass(const Device& device, Batch& batch, const std::filesystem::path& shaderDirectory,
        VkDescriptorSetLayout textureLayout, bool countHits)
        : mBlueNoise(uploadBuffer(device, batch, BlueNoise::shared().getValues(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))
        , mConstants(device, sizeof(Shaders::VisibilityConstants),
              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        , mCountHits(countHits ? 1u : 0u)
        , mPipeline(device, sBindings, 0, std::span(&textureLayout, 1), shaderDirectory / "visibility.comp.spv",
              "visibility", std::span(&mCountHits, 1))
    {
    }

    void VisibilityPass::record(VkCommandBuffer commands, const VisibilityInputs& inputs, const GBuffer& buffer,
        const Buffer& hitCount, const Shaders::VisibilityConstants& constants) const
    {
        assert(buffer.getWidth() >= constants.mCamera.mWidth && buffer.getHeight() >= constants.mCamera.mHeight);

        // **How many emitters there are is the scene's answer and not the camera's.** The table
        // never shrinks, so its length says nothing about this frame; taking the count off the
        // buffers here is what keeps a caller from having to know the table exists at all.
        Shaders::VisibilityConstants described = constants;

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
        const std::array<VkDescriptorImageInfo, 10> channels{
            VkDescriptorImageInfo{ VK_NULL_HANDLE, buffer.getDirect().getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, buffer.getIndirect().getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, buffer.getAlbedo().getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, buffer.getGuide().getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, buffer.getMotion().getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, buffer.getDepth().getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, buffer.getSpecular().getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, buffer.getParticleMask().getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, buffer.getBiasMask().getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, buffer.getReflectionMotion().getView(), VK_IMAGE_LAYOUT_GENERAL },
        };
        // In `sBindings`' order, which is where those numbers are explained.
        constexpr std::array<std::uint32_t, 10> channelBindings{ 1, 15, 17, 18, 20, 22, 26, 27, 28, 29 };
        static_assert(channels.size() == channelBindings.size(), "every channel needs the binding it goes to");

        // Bindings two upwards are all storage buffers, in the order the shader declares them.
        const std::array<VkDescriptorBufferInfo, 13> buffers{
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
            VkDescriptorBufferInfo{ inputs.mBuffers->getWaves(), 0, VK_WHOLE_SIZE },
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

        const auto append = [&](std::uint32_t binding, VkDescriptorType type, const void* next,
                                const VkDescriptorImageInfo* image, const VkDescriptorBufferInfo* block) {
            assert(filled < writes.size() && "more descriptor writes than the layout has bindings");
            writes[filled++] = VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = next,
                .dstBinding = binding,
                .descriptorCount = 1,
                .descriptorType = type,
                .pImageInfo = image,
                .pBufferInfo = block,
            };
        };
        const auto appendImage = [&](std::uint32_t binding, const VkDescriptorImageInfo& image) {
            append(binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, nullptr, &image, nullptr);
        };
        const auto appendBuffer = [&](std::uint32_t binding, const VkDescriptorBufferInfo& block) {
            append(binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, nullptr, &block);
        };
        const auto appendUniform = [&](std::uint32_t binding, const VkDescriptorBufferInfo& block) {
            append(binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, nullptr, &block);
        };

        // The one write whose payload hangs off `pNext` rather than off a pointer field.
        append(0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, &sceneWrite, nullptr, nullptr);

        for (std::uint32_t i = 0; i < channels.size(); ++i)
            appendImage(channelBindings[i], channels[i]);

        for (std::uint32_t i = 0; i < buffers.size(); ++i)
            appendBuffer(i + 2, buffers[i]);

        appendBuffer(16, noiseWrite);
        appendBuffer(19, shadingWrite);
        appendBuffer(21, gridWrite);
        appendBuffer(23, spriteWrite);
        appendBuffer(24, emitterWrite);
        appendUniform(25, frameWrite);
        appendBuffer(30, tileOffsetWrite);
        appendBuffer(31, tileIndexWrite);

        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mPipeline.getHandle());
        // Every binding the layout declares, written exactly once — a shader that grew one and a
        // record that did not is the failure this counts.
        assert(filled == writes.size() && "a binding the layout declares was left unwritten");

        vkCmdPushDescriptorSet(
            commands, VK_PIPELINE_BIND_POINT_COMPUTE, mPipeline.getLayout(), 0, filled, writes.data());
        vkCmdBindDescriptorSets(
            commands, VK_PIPELINE_BIND_POINT_COMPUTE, mPipeline.getLayout(), 1, 1, &inputs.mTextures, 0, nullptr);
        vkCmdDispatch(commands, groupsFor(constants.mCamera.mWidth), groupsFor(constants.mCamera.mHeight), 1);
    }

}
