#include "tracepipeline.hpp"

#include <cassert>
#include <cstring>
#include <vector>

#include "device.hpp"
#include "result.hpp"
#include "shadermodule.hpp"
#include "specialization.hpp"

namespace Rtx
{
    TracePipeline::TracePipeline(const Device& device, std::span<const VkDescriptorSetLayoutBinding> bindings,
        std::span<const VkDescriptorSetLayout> laterSets, const TraceShaders& shaders, std::string_view name,
        std::span<const std::uint32_t> specialization)
        : mDevice(device)
        , mLayout(device, bindings, 0, VK_SHADER_STAGE_RAYGEN_BIT_KHR, laterSets)
    {
        const bool anyHitWanted = !shaders.mAnyHit.empty();

        // **A stage is not a group, which is what the any-hit shader makes true.** One any-hit is
        // compiled and every hit group names it, so the stages run raygen, the miss shaders, that
        // one, and then the closest-hit shaders — while the groups run raygen, the miss records and
        // the hit records. The handles come back in group order, which is the order the table below
        // is filled in.
        std::vector<ShaderModule> compiled;
        compiled.reserve(1 + shaders.mMiss.size() + (anyHitWanted ? 1 : 0) + shaders.mHit.size());

        const Specialization constants(specialization);

        std::vector<VkPipelineShaderStageCreateInfo> stages;
        std::vector<VkRayTracingShaderGroupCreateInfoKHR> groups;
        stages.reserve(compiled.capacity());
        groups.reserve(1 + shaders.mMiss.size() + shaders.mHit.size());

        const auto addStage = [&](VkShaderStageFlagBits stage, const std::filesystem::path& module) {
            const auto at = static_cast<std::uint32_t>(stages.size());
            compiled.emplace_back(device, module);
            stages.push_back(VkPipelineShaderStageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = stage,
                .module = compiled.back().getHandle(),
                .pName = "main",
                .pSpecializationInfo = constants.getInfo(),
            });

            return at;
        };

        const auto addGeneral = [&](std::uint32_t stage) {
            groups.push_back(VkRayTracingShaderGroupCreateInfoKHR{
                .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
                .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
                .generalShader = stage,
                .closestHitShader = VK_SHADER_UNUSED_KHR,
                .anyHitShader = VK_SHADER_UNUSED_KHR,
                .intersectionShader = VK_SHADER_UNUSED_KHR,
            });
        };

        addGeneral(addStage(VK_SHADER_STAGE_RAYGEN_BIT_KHR, shaders.mRaygen));
        for (const std::filesystem::path& module : shaders.mMiss)
            addGeneral(addStage(VK_SHADER_STAGE_MISS_BIT_KHR, module));

        const std::uint32_t anyHit
            = anyHitWanted ? addStage(VK_SHADER_STAGE_ANY_HIT_BIT_KHR, shaders.mAnyHit) : VK_SHADER_UNUSED_KHR;

        for (const std::filesystem::path& module : shaders.mHit)
            groups.push_back(VkRayTracingShaderGroupCreateInfoKHR{
                .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
                .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
                .generalShader = VK_SHADER_UNUSED_KHR,
                .closestHitShader = addStage(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, module),
                .anyHitShader = anyHit,
                .intersectionShader = VK_SHADER_UNUSED_KHR,
            });

        const VkRayTracingPipelineCreateInfoKHR pipeline{
            .sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
            // **Asked for and, on this driver, not answered.** NVIDIA reports one executable for
            // every compute pipeline in this renderer and none at all for a ray tracing one, so the
            // register count the trace was read at as a dispatch is a number this device no longer
            // gives — `Device::reportPipeline` is where that shows. The flag stays because it costs
            // the frame nothing and is what makes the report appear the day a driver answers.
            .flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR,
            .stageCount = static_cast<std::uint32_t>(stages.size()),
            .pStages = stages.data(),
            .groupCount = static_cast<std::uint32_t>(groups.size()),
            .pGroups = groups.data(),
            // **One, and one is the whole of it.** The launch traces a hit object and runs the
            // shader it names; that shader traces again with inline ray queries, which are not
            // recursion and cost the stack nothing. Nothing anywhere calls `traceRayEXT`, so no
            // second level exists to be sized for.
            .maxPipelineRayRecursionDepth = 1,
            .layout = mLayout.getHandle(),
        };
        checkVk(device.getFunctions().mCreateRayTracingPipelines(device.getHandle(), VK_NULL_HANDLE,
                    device.getPipelineCache(), 1, &pipeline, nullptr, mHandle.put(device.getHandle())),
            "vkCreateRayTracingPipelinesKHR");

        device.setName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<std::uint64_t>(mHandle.get()), name);
        device.reportPipeline(mHandle.get(), name);

        const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& limits
            = device.getPhysicalDevice().getProperties().mRayTracingPipeline;

        // One record per group, in the order the groups were appended, packed at the handle's own
        // alignment. What separates the three regions is the coarser base alignment below.
        const VkDeviceSize stride = alignUp(limits.shaderGroupHandleSize, limits.shaderGroupHandleAlignment);

        // **A region's size is its stride for the ray generation stage**, and both are aligned to
        // the base alignment rather than the handle's, which is what makes that one record longer
        // than the two kinds beside it.
        const VkDeviceSize raygenStride = alignUp(stride, limits.shaderGroupBaseAlignment);

        const auto region = [&](VkDeviceSize& at, VkDeviceSize each, std::size_t count) {
            at = alignUp(at, limits.shaderGroupBaseAlignment);
            const VkStridedDeviceAddressRegionKHR described{
                .deviceAddress = at,
                .stride = count > 0 ? each : 0,
                .size = each * count,
            };
            at += described.size;

            return described;
        };

        VkDeviceSize at = 0;
        mRaygen = region(at, raygenStride, 1);
        mMiss = region(at, stride, shaders.mMiss.size());
        mHit = region(at, stride, shaders.mHit.size());

        mTable = Buffer::hostWritten(
            device, at, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        mTable.clear();

        std::vector<std::uint8_t> handles(groups.size() * limits.shaderGroupHandleSize);
        checkVk(device.getFunctions().mGetRayTracingShaderGroupHandles(device.getHandle(), mHandle.get(), 0,
                    static_cast<std::uint32_t>(groups.size()), handles.size(), handles.data()),
            "vkGetRayTracingShaderGroupHandlesKHR");

        // Each region's records, in the order their groups were made: the handles came back packed
        // at the handle size and go out at the stride the region was laid out with. The regions
        // still hold offsets here, which is what a write into the table wants.
        std::uint32_t group = 0;
        const auto fill = [&](const VkStridedDeviceAddressRegionKHR& into, std::size_t count) {
            for (std::size_t record = 0; record < count; ++record, ++group)
                mTable.writeAt(into.deviceAddress + record * into.stride,
                    std::span<const std::uint8_t>(
                        handles.data() + group * limits.shaderGroupHandleSize, limits.shaderGroupHandleSize));
        };
        fill(mRaygen, 1);
        fill(mMiss, shaders.mMiss.size());
        fill(mHit, shaders.mHit.size());

        // Each `Buffer` is its own allocation bound at offset zero, so its address is the
        // allocation's — which every driver hands back far more coarsely aligned than this. Asserted
        // rather than worked around, because a table that has to be offset into is a table this
        // renderer does not have.
        const VkDeviceAddress base = mTable.getDeviceAddress();
        assert(base % limits.shaderGroupBaseAlignment == 0
            && "a shader binding table the device would not read from where it was put");

        // The regions were laid out as offsets and become addresses once there is a buffer under
        // them. An empty region keeps its nought, which is what an unused one is.
        for (VkStridedDeviceAddressRegionKHR* described : { &mRaygen, &mMiss, &mHit })
            described->deviceAddress = described->size > 0 ? base + described->deviceAddress : 0;
    }

    void TracePipeline::traceRays(VkCommandBuffer commands, std::uint32_t width, std::uint32_t height) const
    {
        mDevice.getFunctions().mCmdTraceRays(commands, &mRaygen, &mMiss, &mHit, &mCallable, width, height, 1);
    }
}
