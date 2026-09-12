#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtx/runs.hpp>

#include "buffer.hpp"

namespace Rtx
{
    class Device;

    /// Where one bottom-level structure sits: which block of storage, and the run inside it. Empty
    /// for a mesh slot that has no structure — a slot the scene took back and has not filled.
    struct StructureRoom
    {
        std::uint32_t mBlock = 0;
        Run mRun;

        bool empty() const { return mRun.empty(); }
    };

    /// Room for bottom-level acceleration structures, as a list of buffers nothing ever moves: one
    /// buffer sized to the scene is what made a cell arriving rebuild the world. A `RunAllocator`
    /// over each block, in units of the structure alignment. A room is given back through a
    /// `Graveyard` and never straight to `give`, because a frame that traced the structure may
    /// still be on the queue.
    class StructureStorage
    {
    public:
        /// What an offset in a block has to be a multiple of, and so the unit a room is handed out
        /// in. Vulkan fixes it at 256 for a structure. Public because a caller summing what its
        /// rooms will come to — the `least` it asks a new block for — sums them in this unit.
        static constexpr VkDeviceSize sAlignment = 256;

        /// @param usage what a block's buffer is created with, which is what says who may be placed
        ///        in it. What is stored this way is an opaque object the driver puts at a 256-byte
        ///        offset in a buffer the application owns.
        /// @param name what a block is called in a capture, numbered from there.
        StructureStorage(VkBufferUsageFlags usage, std::string name);

        /// Room for a structure of `bytes`, taken from the first block that has it.
        ///
        /// @param least how large to make a new block where none of the existing ones can hold it:
        ///        a load asks for the whole scene's total, an arrival for nothing in particular.
        StructureRoom take(const Device& device, VkDeviceSize bytes, VkDeviceSize least);

        /// Gives a structure's room back, and gives the block to the device where that was the last
        /// room in it. The structure itself is the caller's to destroy, and a `Graveyard` destroys
        /// every structure it holds before it gives back a single room.
        void give(const StructureRoom& room);

        VkBuffer getBuffer(const StructureRoom& room) const { return mBlocks[room.mBlock].mBuffer.getHandle(); }
        VkDeviceSize getOffset(const StructureRoom& room) const;

        /// How much storage exists, which is what the device was asked for.
        VkDeviceSize getBytes() const;

        /// What the structures standing in it occupy, beside the room asked for, because the two
        /// part company once anything is compacted.
        VkDeviceSize getLiveBytes() const;

    private:
        /// One buffer and what has been handed out inside it. The allocator has no block boundary
        /// of its own: this buffer *is* the block, and what stops a run leaving it is the capacity
        /// checked against `getEnd`.
        struct Block
        {
            Buffer mBuffer;
            RunAllocator mRuns;

            /// How long the block is, and nought where it has been given back: a retired block
            /// keeps its place so that no room is renumbered, and `take` fills the place again.
            std::uint32_t mUnits = 0;
        };

        /// How many places hold a block, which is what says whether this is the last one.
        std::size_t countLive() const;

        std::vector<Block> mBlocks;

        VkBufferUsageFlags mUsage = 0;
        std::string mName;
    };
}
