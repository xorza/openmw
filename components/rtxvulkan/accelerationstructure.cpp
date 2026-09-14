#include "accelerationstructure.hpp"

#include <utility>

#include "buffer.hpp"
#include "device.hpp"
#include "result.hpp"

namespace Rtx
{
    AccelerationStructure::AccelerationStructure(const Device& device, const VkAccelerationStructureTypeKHR type,
        const VkBuffer buffer, const VkDeviceSize offset, const VkDeviceSize size, StructureStorage* const storage,
        const StructureRoom& room)
        : mDevice(&device)
        , mStorage(storage)
        , mRoom(room)
    {
        const DeviceFunctions& functions = device.getFunctions();

        const VkAccelerationStructureCreateInfoKHR create{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
            .buffer = buffer,
            .offset = offset,
            .size = size,
            .type = type,
        };
        checkVk(functions.mCreateAccelerationStructure(device.getHandle(), &create, nullptr, &mHandle),
            "vkCreateAccelerationStructureKHR");

        // Asked before anything is built into it: an address belongs to the structure from the
        // moment it is created, and what a barrier orders is the contents arriving.
        const VkAccelerationStructureDeviceAddressInfoKHR address{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
            .accelerationStructure = mHandle,
        };
        mAddress = functions.mGetAccelerationStructureDeviceAddress(device.getHandle(), &address);
    }

    AccelerationStructure AccelerationStructure::bottomLevel(
        const Device& device, StructureStorage& storage, const StructureRoom& room, const VkDeviceSize size)
    {
        return AccelerationStructure(device, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, storage.getBuffer(room),
            storage.getOffset(room), size, &storage, room);
    }

    AccelerationStructure AccelerationStructure::topLevel(
        const Device& device, const Buffer& storage, const VkDeviceSize size, const std::string_view name)
    {
        AccelerationStructure made(
            device, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, storage.getHandle(), 0, size, nullptr, {});
        device.setName(made.mHandle, name);
        return made;
    }

    AccelerationStructure::~AccelerationStructure()
    {
        reset();
    }

    AccelerationStructure::AccelerationStructure(AccelerationStructure&& other) noexcept
        : mDevice(other.mDevice)
        , mHandle(std::exchange(other.mHandle, VK_NULL_HANDLE))
        , mAddress(std::exchange(other.mAddress, 0))
        , mStorage(other.mStorage)
        , mRoom(std::exchange(other.mRoom, StructureRoom{}))
    {
    }

    AccelerationStructure& AccelerationStructure::operator=(AccelerationStructure&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            mDevice = other.mDevice;
            mHandle = std::exchange(other.mHandle, VK_NULL_HANDLE);
            mAddress = std::exchange(other.mAddress, 0);
            mStorage = other.mStorage;
            mRoom = std::exchange(other.mRoom, StructureRoom{});
        }

        return *this;
    }

    void AccelerationStructure::reset()
    {
        // The handle before the room: a room given back is the next structure's, and one given
        // back under a structure still standing is two of them in one place.
        if (mHandle != VK_NULL_HANDLE)
            mDevice->getFunctions().mDestroyAccelerationStructure(mDevice->getHandle(), mHandle, nullptr);
        if (mStorage != nullptr)
            mStorage->give(mRoom);

        mHandle = VK_NULL_HANDLE;
        mAddress = 0;
        mRoom = StructureRoom{};
    }
}
