#include "buffer.hpp"

#include <cassert>
#include <utility>

#include "device.hpp"
#include "result.hpp"

namespace Rtx
{
    Buffer::Buffer(const Device& device, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties)
        : mDevice(device.getHandle())
        , mSize(size)
        , mAddressable((usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0)
    {
        assert(size > 0);

        const VkBufferCreateInfo create{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        checkVk(vkCreateBuffer(mDevice, &create, nullptr, &mHandle), "vkCreateBuffer");

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(mDevice, mHandle, &requirements);

        mMemory = DeviceMemory(device, requirements.size, requirements.memoryTypeBits, properties, mAddressable);
        checkVk(vkBindBufferMemory(mDevice, mHandle, mMemory.getHandle(), 0), "vkBindBufferMemory");
    }

    Buffer::~Buffer()
    {
        destroy();
    }

    Buffer::Buffer(Buffer&& other) noexcept
        : mDevice(other.mDevice)
        , mHandle(std::exchange(other.mHandle, VK_NULL_HANDLE))
        , mMemory(std::move(other.mMemory))
        , mSize(other.mSize)
        , mAddressable(other.mAddressable)
    {
    }

    Buffer& Buffer::operator=(Buffer&& other) noexcept
    {
        if (this != &other)
        {
            destroy();
            mDevice = other.mDevice;
            mHandle = std::exchange(other.mHandle, VK_NULL_HANDLE);
            mMemory = std::move(other.mMemory);
            mSize = other.mSize;
            mAddressable = other.mAddressable;
        }
        return *this;
    }

    VkDeviceAddress Buffer::getDeviceAddress() const
    {
        assert(mAddressable);

        const VkBufferDeviceAddressInfo info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .buffer = mHandle,
        };
        return vkGetBufferDeviceAddress(mDevice, &info);
    }

    void Buffer::destroy()
    {
        if (mHandle != VK_NULL_HANDLE)
            vkDestroyBuffer(mDevice, mHandle, nullptr);
        mHandle = VK_NULL_HANDLE;
        mMemory = DeviceMemory();
    }
}
