#pragma once

#include <vulkan/vulkan_core.h>

namespace Rtx
{
    /// What each kind of buffer this renderer makes is created able to do, named once: a usage
    /// re-declared per file is one that drifts where nothing decides.

    /// A table a shader reaches through an address in the frame block. Addressable and never
    /// bound: no descriptor names one of these.
    inline constexpr VkBufferUsageFlags sTableUsage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    /// The one table a trace's bin copies *out of* on the device: the placement's sprites.
    inline constexpr VkBufferUsageFlags sTableCopiedFromUsage = sTableUsage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    /// A table the device fills — the bin's own sprites, copied into, and its list, whose head a
    /// fill zeroes before every bin.
    inline constexpr VkBufferUsageFlags sTableFilledUsage = sTableUsage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    /// Addressable as well as build input, because the shader reads the indices back at a hit
    /// through the same address the build read them at, and there is no reason for a second copy
    /// of them to exist. No descriptor names any of these, so nothing else is asked for.
    inline constexpr VkBufferUsageFlags sBuildInputUsage
        = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
        | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    /// What a build works in, and never reads again.
    inline constexpr VkBufferUsageFlags sScratchUsage
        = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    /// Where a structure stands: an opaque object the driver puts at a 256-byte offset in a buffer
    /// the application owns.
    inline constexpr VkBufferUsageFlags sStructureStorageUsage
        = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    /// What a structure's offset in its buffer has to be a multiple of. Vulkan fixes it at 256, and
    /// it is only enough where the buffer itself starts on the boundary, which `Buffer` sees to.
    inline constexpr VkDeviceSize sStructureOffsetAlignment = 256;
}
