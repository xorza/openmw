#pragma once

#include <filesystem>

#include <vulkan/vulkan_core.h>

namespace Rtx
{
    class Device;

    /// A `VkShaderModule` built from a SPIR-V file the build produced.
    ///
    /// The build compiles every shader with `glslc` and runs `spirv-val` over the result, so a
    /// module that reaches here has already been validated. What this checks is that the *file* is
    /// the one the build wrote — a stale or truncated `.spv` is otherwise a driver crash with no
    /// explanation.
    class ShaderModule
    {
    public:
        ShaderModule(const Device& device, const std::filesystem::path& path);
        ~ShaderModule();

        ShaderModule(const ShaderModule&) = delete;
        ShaderModule& operator=(const ShaderModule&) = delete;
        ShaderModule(ShaderModule&& other) noexcept;
        ShaderModule& operator=(ShaderModule&& other) noexcept;

        VkShaderModule getHandle() const { return mHandle; }

    private:
        VkDevice mDevice = VK_NULL_HANDLE;
        VkShaderModule mHandle = VK_NULL_HANDLE;
    };
}
