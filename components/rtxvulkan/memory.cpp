#include "memory.hpp"

#include <string>
#include <utility>

#include <components/rtx/error.hpp>

#include "device.hpp"
#include "result.hpp"

namespace Rtx
{
    std::uint32_t findMemoryType(const Device& device, std::uint32_t typeBits, VkMemoryPropertyFlags properties)
    {
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(device.getPhysicalDevice().getHandle(), &memory);

        for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        {
            const bool allowed = (typeBits & (1u << i)) != 0;
            const bool suitable = (memory.memoryTypes[i].propertyFlags & properties) == properties;
            if (allowed && suitable)
                return i;
        }

        throw Unsupported("no memory type has properties " + std::to_string(properties) + " among the "
            + std::to_string(memory.memoryTypeCount) + " this device offers");
    }

    DeviceMemory::DeviceMemory(const Device& device, VkDeviceSize size, std::uint32_t typeBits,
        VkMemoryPropertyFlags properties, bool deviceAddress)
    {
        const VkMemoryAllocateFlagsInfo flags{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
            .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
        };

        const VkMemoryAllocateInfo allocate{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = deviceAddress ? &flags : nullptr,
            .allocationSize = size,
            .memoryTypeIndex = findMemoryType(device, typeBits, properties),
        };

        checkVk(vkAllocateMemory(device.getHandle(), &allocate, nullptr, mHandle.put(device.getHandle())),
            "vkAllocateMemory");

        // **Mapped here rather than by whoever holds this**, so the pointer goes when the allocation
        // does: a buffer that kept its own would hand out an address into memory it had moved away.
        if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0)
            checkVk(vkMapMemory(device.getHandle(), mHandle.get(), 0, VK_WHOLE_SIZE, 0, &mMapped), "vkMapMemory");
    }

    DeviceMemory::DeviceMemory(DeviceMemory&& other) noexcept
        : mHandle(std::move(other.mHandle))
        , mMapped(std::exchange(other.mMapped, nullptr))
    {
    }

    DeviceMemory& DeviceMemory::operator=(DeviceMemory&& other) noexcept
    {
        if (this != &other)
        {
            mHandle = std::move(other.mHandle);
            mMapped = std::exchange(other.mMapped, nullptr);
        }

        return *this;
    }
}
