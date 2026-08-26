#include "memory.hpp"

#include <cassert>
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

        throw Error("no memory type has properties " + std::to_string(properties) + " among the "
            + std::to_string(memory.memoryTypeCount) + " this device offers");
    }

    DeviceMemory::DeviceMemory(const Device& device, VkDeviceSize size, std::uint32_t typeBits,
        VkMemoryPropertyFlags properties, bool deviceAddress)
        : mDevice(device.getHandle())
        , mHostVisible((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0)
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

        checkVk(vkAllocateMemory(mDevice, &allocate, nullptr, &mHandle), "vkAllocateMemory");
    }

    DeviceMemory::~DeviceMemory()
    {
        destroy();
    }

    DeviceMemory::DeviceMemory(DeviceMemory&& other) noexcept
        : mDevice(other.mDevice)
        , mHandle(std::exchange(other.mHandle, VK_NULL_HANDLE))
        , mHostVisible(other.mHostVisible)
    {
    }

    DeviceMemory& DeviceMemory::operator=(DeviceMemory&& other) noexcept
    {
        if (this != &other)
        {
            destroy();
            mDevice = other.mDevice;
            mHandle = std::exchange(other.mHandle, VK_NULL_HANDLE);
            mHostVisible = other.mHostVisible;
        }
        return *this;
    }

    void* DeviceMemory::map() const
    {
        assert(mHostVisible);

        void* mapped = nullptr;
        checkVk(vkMapMemory(mDevice, mHandle, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");
        return mapped;
    }

    void DeviceMemory::unmap() const
    {
        vkUnmapMemory(mDevice, mHandle);
    }

    void DeviceMemory::destroy()
    {
        if (mHandle != VK_NULL_HANDLE)
            vkFreeMemory(mDevice, mHandle, nullptr);
        mHandle = VK_NULL_HANDLE;
    }
}
