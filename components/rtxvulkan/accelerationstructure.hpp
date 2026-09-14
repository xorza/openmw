#pragma once

#include <cstdint>
#include <string_view>

#include <vulkan/vulkan_core.h>

#include "structurestorage.hpp"

namespace Rtx
{
    class Buffer;
    class Device;

    /// A `VkAccelerationStructureKHR`, its device address, and the room it stands in: the one
    /// device handle `Owned` cannot hold, because its destroyer is a pointer the device loaded
    /// and not a function the header declares. Made once with its address asked once — a handle
    /// lasts until the mesh it belongs to is released, and the alternative was the same question
    /// per instance per frame. Destroying one gives its room back after the handle has gone, so
    /// two structures never stand in one place.
    class AccelerationStructure
    {
    public:
        /// A slot with nothing in it: a mesh the scene took back and has not filled.
        AccelerationStructure() = default;

        /// A bottom level standing in `room` of `storage`, which is given back when this goes.
        static AccelerationStructure bottomLevel(
            const Device& device, StructureStorage& storage, const StructureRoom& room, VkDeviceSize size);

        /// A top level at the start of `storage`, which is the caller's to keep.
        static AccelerationStructure topLevel(
            const Device& device, const Buffer& storage, VkDeviceSize size, std::string_view name);

        ~AccelerationStructure();

        AccelerationStructure(const AccelerationStructure&) = delete;
        AccelerationStructure& operator=(const AccelerationStructure&) = delete;

        AccelerationStructure(AccelerationStructure&& other) noexcept;
        AccelerationStructure& operator=(AccelerationStructure&& other) noexcept;

        VkAccelerationStructureKHR getHandle() const { return mHandle; }
        VkDeviceAddress getAddress() const { return mAddress; }

        bool isEmpty() const { return mHandle == VK_NULL_HANDLE; }

    private:
        AccelerationStructure(const Device& device, VkAccelerationStructureTypeKHR type, VkBuffer buffer,
            VkDeviceSize offset, VkDeviceSize size, StructureStorage* storage, const StructureRoom& room);

        void reset();

        const Device* mDevice = nullptr;
        VkAccelerationStructureKHR mHandle = VK_NULL_HANDLE;
        VkDeviceAddress mAddress = 0;

        /// Null for a top level, whose storage is a buffer of its own.
        StructureStorage* mStorage = nullptr;
        StructureRoom mRoom;
    };
}
