#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtx/spanallocator.hpp>

#include "buffer.hpp"

namespace Rtx
{
    class Device;

    /// Where one bottom-level structure sits: which block of storage, and the run inside it.
    ///
    /// Empty for a mesh slot that has no structure — a slot the scene took back and has not filled.
    struct StructureRoom
    {
        std::uint32_t mBlock = 0;
        Span mRun;

        bool empty() const { return mRun.empty(); }
    };

    /// Room for bottom-level acceleration structures, as a list of buffers nothing ever moves.
    ///
    /// **One buffer sized to the scene is what made a cell arriving rebuild the world.** A structure
    /// lives at an offset in a buffer and is only valid while that buffer is; sizing the buffer to
    /// the scene meant a scene that grew got a new one, and every structure in it had to be created
    /// and built again. Here the buffers are only ever added to the list, so a structure made in one
    /// outlives every later arrival.
    ///
    /// **A `SpanAllocator` over each block, in units of the structure alignment.** A released mesh
    /// gives its run back and the next structure that fits takes it, which is the same "slots, not
    /// compaction" rule the scene itself is built on — nothing is moved, so nothing is renumbered.
    ///
    /// The renderer is synchronous, so a structure destroyed after a release cannot be in flight.
    /// When the frame stops waiting on the queue this is one of the places that has to grow a
    /// fence-keyed retirement list.
    class StructureStorage
    {
    public:
        /// @param usage what a block is created with. The two things stored this way — bottom-level
        ///        structures and opacity micromaps — differ in one usage bit and in nothing else:
        ///        both are opaque objects the driver places at a 256-byte offset in a buffer the
        ///        application owns.
        /// @param name what a block is called in a capture, numbered from there.
        StructureStorage(VkBufferUsageFlags usage, std::string name);

        /// Room for a structure of `bytes`, taken from the first block that has it.
        ///
        /// @param least how large to make a new block where none of the existing ones can hold it.
        ///        A load knows the whole scene's total and asks for it, so a cell's structures land
        ///        in one allocation; an arrival asks for nothing in particular and gets a block big
        ///        enough for itself.
        StructureRoom take(const Device& device, VkDeviceSize bytes, VkDeviceSize least);

        /// Gives a structure's room back. The structure itself is the caller's to destroy.
        void give(const StructureRoom& room);

        VkBuffer getBuffer(const StructureRoom& room) const { return mBlocks[room.mBlock].mBuffer.getHandle(); }
        VkDeviceSize getOffset(const StructureRoom& room) const;

        /// How much storage exists, which is what a scene reports as its structures' size.
        VkDeviceSize getBytes() const;

    private:
        /// One buffer and what has been handed out inside it.
        ///
        /// The allocator has no block boundary of its own: this buffer *is* the block, and what
        /// stops a run leaving it is the capacity checked against `getEnd`.
        struct Block
        {
            Buffer mBuffer;
            SpanAllocator mRuns;
            std::uint32_t mUnits = 0;
        };

        std::vector<Block> mBlocks;

        VkBufferUsageFlags mUsage = 0;
        std::string mName;
    };
}
