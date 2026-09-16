#pragma once

#include <string_view>

#include <vulkan/vulkan_core.h>

#include "readstamp.hpp"
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
    /// two structures never stand in one place — and asserts `isIdle`, as a buffer's does. A
    /// bottom level is read by every top-level build without being named again; the store it
    /// stands in carries that naming for all of its rows, and a row goes through the graveyard,
    /// which stamps after every top level made.
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

        /// The handle, and a hand-out: names the structure for the next submit, as a build, a
        /// copy, a query and a launch all take it. Null for an empty slot, which nothing can hand
        /// a submit and so names nothing.
        VkAccelerationStructureKHR getHandle() const
        {
            if (!isEmpty())
                nameForNext();
            return mHandle;
        }

        VkDeviceAddress getAddress() const { return mAddress; }

        bool isEmpty() const { return mHandle == VK_NULL_HANDLE; }

        /// Names the structure for the next submit where a caller reaches it through a handle it
        /// kept — a top-level build reuses the description it was made with.
        void nameForNext() const;

        /// Whether every submit that names this structure has run — what the destructor asserts,
        /// and what the graveyard asserts as it frees.
        bool isIdle() const;

    private:
        AccelerationStructure(const Device& device, VkAccelerationStructureTypeKHR type, VkBuffer buffer,
            VkDeviceSize offset, VkDeviceSize size, StructureStorage* storage, const StructureRoom& room);

        void reset();

        const Device* mDevice = nullptr;
        ReadStamp mRead;
        VkAccelerationStructureKHR mHandle = VK_NULL_HANDLE;
        VkDeviceAddress mAddress = 0;

        /// Null for a top level, whose storage is a buffer of its own.
        StructureStorage* mStorage = nullptr;
        StructureRoom mRoom;
    };
}
