#include "memory.hpp"

#include <cassert>
#include <string>
#include <utility>

#include <unistd.h>

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
        VkMemoryPropertyFlags properties, bool deviceAddress, bool exportable)
        : mDevice(device.getHandle())
        , mSize(size)
        , mHostVisible((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0)
        , mExportable(exportable)
    {
        void* next = nullptr;

        VkExportMemoryAllocateInfo shared{
            .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
        };
        if (exportable)
        {
            shared.pNext = next;
            next = &shared;
        }

        VkMemoryAllocateFlagsInfo flags{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
            .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
        };
        if (deviceAddress)
        {
            flags.pNext = next;
            next = &flags;
        }

        const VkMemoryAllocateInfo allocate{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = next,
            .allocationSize = size,
            .memoryTypeIndex = findMemoryType(device, typeBits, properties),
        };

        checkVk(vkAllocateMemory(mDevice, &allocate, nullptr, &mHandle), "vkAllocateMemory");
    }

    DeviceMemory::DeviceMemory(const Device& device, VkDeviceSize size, std::uint32_t typeBits, int fd)
        : mDevice(device.getHandle())
        , mSize(size)
    {
        // **`vkGetMemoryFdPropertiesKHR` is not how an opaque descriptor is placed**, and the spec
        // says so outright: `VUID-vkGetMemoryFdPropertiesKHR-handleType-00674` forbids asking it
        // about `OPAQUE_FD` at all. An opaque handle is only importable by the same driver that
        // exported it, so the memory type is the importing image's own — the question the call
        // answers is one that only arises for the handle types another API allocated.
        const VkImportMemoryFdInfoKHR from{
            .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
            .fd = fd,
        };

        const VkMemoryAllocateInfo allocate{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &from,
            .allocationSize = size,
            .memoryTypeIndex = findMemoryType(device, typeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
        };

        const VkResult imported = vkAllocateMemory(mDevice, &allocate, nullptr, &mHandle);
        if (imported != VK_SUCCESS)
        {
            // Vulkan takes the descriptor only when it succeeds, so a failure leaves it ours.
            ::close(fd);
            checkVk(imported, "vkAllocateMemory importing a descriptor");
        }
    }

    int DeviceMemory::exportFd() const
    {
        assert(mExportable && "this allocation was not made shareable");

        const auto getMemoryFd
            = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(mDevice, "vkGetMemoryFdKHR"));
        if (getMemoryFd == nullptr)
            throw Error("this device has no VK_KHR_external_memory_fd, so nothing can import its images");

        const VkMemoryGetFdInfoKHR request{
            .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
            .memory = mHandle,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
        };

        int handle = -1;
        checkVk(getMemoryFd(mDevice, &request, &handle), "vkGetMemoryFdKHR");
        return handle;
    }

    DeviceMemory::~DeviceMemory()
    {
        destroy();
    }

    DeviceMemory::DeviceMemory(DeviceMemory&& other) noexcept
        : mDevice(other.mDevice)
        , mHandle(std::exchange(other.mHandle, VK_NULL_HANDLE))
        , mSize(other.mSize)
        , mHostVisible(other.mHostVisible)
        , mExportable(other.mExportable)
    {
    }

    DeviceMemory& DeviceMemory::operator=(DeviceMemory&& other) noexcept
    {
        if (this != &other)
        {
            destroy();
            mDevice = other.mDevice;
            mHandle = std::exchange(other.mHandle, VK_NULL_HANDLE);
            mSize = other.mSize;
            mHostVisible = other.mHostVisible;
            mExportable = other.mExportable;
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
