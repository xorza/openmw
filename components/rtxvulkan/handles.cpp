#include "handles.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include <components/files/conversion.hpp>
#include <components/rtx/error.hpp>

#include "device.hpp"
#include "result.hpp"

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

        ShaderModule handle;
        checkVk(vkCreateShaderModule(device.getHandle(), &createInfo, nullptr, handle.put(device.getHandle())),
            "vkCreateShaderModule");

        // The name is built from a path, so it can throw — and a handle already made is destroyed
        // on the way out.
        device.setName(VK_OBJECT_TYPE_SHADER_MODULE, reinterpret_cast<std::uint64_t>(handle.get()),
            Files::pathToUnicodeString(path.filename()).c_str());

        return handle;
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
        SetLayout handle;
        checkVk(vkCreateDescriptorSetLayout(device.getHandle(), &describe, nullptr, handle.put(device.getHandle())),
            "vkCreateDescriptorSetLayout");

        return handle;
    }

    namespace
    {
        Sampler createSampler(const Device& device, const VkSamplerCreateInfo& describe, std::string_view name)
        {
            Sampler handle;
            checkVk(vkCreateSampler(device.getHandle(), &describe, nullptr, handle.put(device.getHandle())),
                "vkCreateSampler");
            device.setName(VK_OBJECT_TYPE_SAMPLER, reinterpret_cast<std::uint64_t>(handle.get()), name);
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
        std::uint32_t pushConstantBytes, VkShaderStageFlags pushStages,
        std::span<const VkDescriptorSetLayout> laterSets)
        : mSetLayout(makeSetLayout(device, bindings, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT))
    {
        // Set zero is this pipeline's own; whatever the caller named follows it, in order.
        std::vector<VkDescriptorSetLayout> sets;
        sets.reserve(laterSets.size() + 1);
        sets.push_back(mSetLayout.get());
        sets.insert(sets.end(), laterSets.begin(), laterSets.end());

        const VkPushConstantRange range{
            .stageFlags = pushStages,
            .size = pushConstantBytes,
        };
        const bool pushes = pushConstantBytes > 0;
        const VkPipelineLayoutCreateInfo pipelineLayout{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = static_cast<std::uint32_t>(sets.size()),
            .pSetLayouts = sets.data(),
            .pushConstantRangeCount = pushes ? 1u : 0u,
            .pPushConstantRanges = pushes ? &range : nullptr,
        };
        checkVk(vkCreatePipelineLayout(device.getHandle(), &pipelineLayout, nullptr, mHandle.put(device.getHandle())),
            "vkCreatePipelineLayout");
    }
}
