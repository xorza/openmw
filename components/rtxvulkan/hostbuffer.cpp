#include "hostbuffer.hpp"

#include <algorithm>

#include <utility>

#include "device.hpp"
#include "result.hpp"

namespace Rtx
{
    namespace
    {
        /// Video memory the host can write. Required rather than fallen back from — see `Requirements`.
        constexpr VkMemoryPropertyFlags sResizableBar = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
            | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }

    HostBuffer::HostBuffer(const Device& device, VkDeviceSize size, VkBufferUsageFlags usage)
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

        mMemory = DeviceMemory(device, requirements.size, requirements.memoryTypeBits, sResizableBar, mAddressable);
        checkVk(vkBindBufferMemory(mDevice, mHandle, mMemory.getHandle(), 0), "vkBindBufferMemory");

        mMapped = mMemory.map();
    }

    HostBuffer::~HostBuffer()
    {
        destroy();
    }

    HostBuffer::HostBuffer(HostBuffer&& other) noexcept
        : mDevice(other.mDevice)
        , mHandle(std::exchange(other.mHandle, VK_NULL_HANDLE))
        , mMemory(std::move(other.mMemory))
        , mSize(other.mSize)
        , mMapped(std::exchange(other.mMapped, nullptr))
        , mAddressable(other.mAddressable)
    {
    }

    HostBuffer& HostBuffer::operator=(HostBuffer&& other) noexcept
    {
        if (this != &other)
        {
            destroy();
            mDevice = other.mDevice;
            mHandle = std::exchange(other.mHandle, VK_NULL_HANDLE);
            mMemory = std::move(other.mMemory);
            mSize = other.mSize;
            mMapped = std::exchange(other.mMapped, nullptr);
            mAddressable = other.mAddressable;
        }

        return *this;
    }

    VkDeviceAddress HostBuffer::getDeviceAddress() const
    {
        assert(mAddressable);

        const VkBufferDeviceAddressInfo info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .buffer = mHandle,
        };
        return vkGetBufferDeviceAddress(mDevice, &info);
    }

    void HostBuffer::destroy()
    {
        // The mapping goes with the allocation, so there is nothing to unmap: `DeviceMemory`'s
        // destructor frees memory that was never told it had a pointer out.
        mMapped = nullptr;

        if (mHandle != VK_NULL_HANDLE)
            vkDestroyBuffer(mDevice, mHandle, nullptr);

        mHandle = VK_NULL_HANDLE;
        mMemory = DeviceMemory();
    }

    void growTo(HostBuffer& held, const Device& device, VkDeviceSize bytes, VkBufferUsageFlags usage)
    {
        // **One byte and not none.** Vulkan has no zero-sized buffer, so a table with nothing in it
        // still gets the smallest one that can be bound — which is what the shader's descriptor
        // needs and what nothing in it has to read.
        const VkDeviceSize wanted = std::max(bytes, VkDeviceSize{ 1 });
        if (held.getSize() >= wanted)
            return;

        held = HostBuffer(device, wanted, usage);
    }
}
