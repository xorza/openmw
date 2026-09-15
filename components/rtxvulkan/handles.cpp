#include "handles.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <string>
#include <vector>

#include <components/files/conversion.hpp>
#include <components/rtx/error.hpp>

#include "device.hpp"

namespace Rtx
{
    namespace
    {
        constexpr std::uint32_t sSpirvMagic = 0x07230203;

        std::vector<std::uint32_t> readSpirv(const std::filesystem::path& path)
        {
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            if (!stream)
                throw Error("cannot open " + Files::pathToUnicodeString(path));

            const std::streamsize size = stream.tellg();
            if (size <= 0 || size % 4 != 0)
                throw Error(Files::pathToUnicodeString(path) + " is " + std::to_string(size)
                    + " bytes, which is not a whole number of SPIR-V words");

            std::vector<std::uint32_t> words(static_cast<std::size_t>(size) / 4);
            stream.seekg(0);
            stream.read(reinterpret_cast<char*>(words.data()), size);
            if (!stream)
                throw Error("cannot read " + Files::pathToUnicodeString(path));

            if (words.front() != sSpirvMagic)
                throw Error(Files::pathToUnicodeString(path) + " does not begin with the SPIR-V magic number");

            return words;
        }
    }

    ShaderModule loadShaderModule(const Device& device, const std::filesystem::path& path)
    {
        const std::vector<std::uint32_t> words = readSpirv(path);

        const VkShaderModuleCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = words.size() * sizeof(std::uint32_t),
            .pCode = words.data(),
        };

        ShaderModule handle
            = ShaderModule::make(device.getHandle(), vkCreateShaderModule, createInfo, "vkCreateShaderModule");

        // The name is built from a path, so it can throw — and a handle already made is destroyed
        // on the way out.
        device.setName(handle.get(), Files::pathToUnicodeString(path.filename()).c_str());

        return handle;
    }

    Semaphore makeSemaphore(const Device& device)
    {
        const VkSemaphoreCreateInfo create{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        return Semaphore::make(device.getHandle(), vkCreateSemaphore, create, "vkCreateSemaphore");
    }

    Semaphore makeTimelineSemaphore(const Device& device, const std::string_view name)
    {
        const VkSemaphoreTypeCreateInfo type{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
            .initialValue = 0,
        };
        const VkSemaphoreCreateInfo create{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &type,
        };
        Semaphore handle = Semaphore::make(device.getHandle(), vkCreateSemaphore, create, "vkCreateSemaphore");
        device.setName(handle.get(), name);
        return handle;
    }

    Fence makeSignalledFence(const Device& device)
    {
        const VkFenceCreateInfo create{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        return Fence::make(device.getHandle(), vkCreateFence, create, "vkCreateFence");
    }

    SetLayout makeSetLayout(const Device& device, std::span<const VkDescriptorSetLayoutBinding> bindings,
        VkDescriptorSetLayoutCreateFlags flags, const void* next)
    {
        const VkDescriptorSetLayoutCreateInfo describe{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = next,
            .flags = flags,
            .bindingCount = static_cast<std::uint32_t>(bindings.size()),
            .pBindings = bindings.data(),
        };
        return SetLayout::make(
            device.getHandle(), vkCreateDescriptorSetLayout, describe, "vkCreateDescriptorSetLayout");
    }

    namespace
    {
        Sampler createSampler(const Device& device, const VkSamplerCreateInfo& describe, std::string_view name)
        {
            Sampler handle = Sampler::make(device.getHandle(), vkCreateSampler, describe, "vkCreateSampler");
            device.setName(handle.get(), name);
            return handle;
        }
    }

    Sampler makeTargetSampler(const Device& device, std::string_view name)
    {
        const VkSamplerCreateInfo describe{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .maxLod = VK_LOD_CLAMP_NONE,
        };

        return createSampler(device, describe, name);
    }

    Sampler makeContentSampler(const Device& device, std::string_view name)
    {
        const VkSamplerCreateInfo describe{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            // Off, and not an oversight: every fetch names its own level, and anisotropic filtering
            // only applies to the implicit and gradient forms. A cone is isotropic by construction.
            .anisotropyEnable = VK_FALSE,
            .maxLod = VK_LOD_CLAMP_NONE,
        };

        return createSampler(device, describe, name);
    }

    PipelineLayout::PipelineLayout(const Device& device, std::span<const VkDescriptorSetLayoutBinding> bindings,
        const VkPushConstantRange& push, std::span<const VkDescriptorSetLayout> laterSets)
        : mSetLayout(makeSetLayout(device, bindings, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT))
        , mPush(push)
        , mLaterSets(static_cast<std::uint32_t>(laterSets.size()))
    {
        assert(push.offset == 0 && "a push range that does not start at nought");

        // Set zero is this pipeline's own; whatever the caller named follows it, in order.
        std::vector<VkDescriptorSetLayout> sets;
        sets.reserve(laterSets.size() + 1);
        sets.push_back(mSetLayout.get());
        sets.insert(sets.end(), laterSets.begin(), laterSets.end());

        const bool pushes = push.size > 0;
        const VkPipelineLayoutCreateInfo pipelineLayout{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = static_cast<std::uint32_t>(sets.size()),
            .pSetLayouts = sets.data(),
            .pushConstantRangeCount = pushes ? 1u : 0u,
            .pPushConstantRanges = pushes ? &push : nullptr,
        };
        mHandle = Owned<VkPipelineLayout, vkDestroyPipelineLayout>::make(
            device.getHandle(), vkCreatePipelineLayout, pipelineLayout, "vkCreatePipelineLayout");
    }
}
