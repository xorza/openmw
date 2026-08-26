#pragma once

#include <cassert>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "hostbuffer.hpp"

namespace Rtx
{
    class Device;

    /// One table of fixed-size elements, kept as a list of separate buffers of a fixed count each.
    ///
    /// **What makes a scene appendable.** A table sized to the scene has to be made again when the
    /// scene grows, and everything already built from it — an acceleration structure holds the
    /// address of the vertices it was built from — is then pointing at memory that has moved. A
    /// block is allocated once at its full size and never reallocated, so every address handed out
    /// stays good and a cell arriving adds a block rather than replacing the world.
    ///
    /// **Full blocks even where the scene stops part way into the last one**, which is the whole of
    /// what the paragraph above buys: a block cut to what is currently in it would have to be made
    /// again the moment anything more arrived. The slack is bounded by one block per table.
    ///
    /// **Host-written, and every one of these is.** Resizable BAR makes the whole of video memory
    /// writable by the processor, so a mesh arriving is a `memcpy` into the memory the device will
    /// read — no staging buffer, no copy to record, no transfer to order against the build that
    /// reads it. That is what lets an arrival be written without touching what is already there.
    ///
    /// `Rtx::SceneDesc` never lets a mesh's run straddle a block, so `addressOf` on a run's first
    /// element covers the whole run.
    class BlockedBuffer
    {
    public:
        /// @param blockSize elements in a block, which the shader knows as `VERTEX_BLOCK` or
        ///        `INDEX_BLOCK` and divides by.
        /// @param stride bytes an element occupies.
        BlockedBuffer(std::uint32_t blockSize, std::uint32_t stride)
            : mBlockSize(blockSize)
            , mStride(stride)
        {
            assert(blockSize > 0 && stride > 0);
        }

        /// Says which device the blocks are made on and what they are for. Once, before anything is
        /// reserved.
        void open(const Device& device, VkBufferUsageFlags usage, std::string_view name);

        std::uint32_t getBlockSize() const { return mBlockSize; }
        std::uint32_t getStride() const { return mStride; }

        /// Bytes one whole block occupies, which is what every block is made at.
        VkDeviceSize getBlockBytes() const { return VkDeviceSize{ mBlockSize } * mStride; }

        /// Makes blocks until the table can hold `elements`, and rewrites the address table where it
        /// made any. Nothing already in it moves. At least one block always exists, because a table
        /// nothing has been put in still has to be bound.
        void reserve(std::uint32_t elements);

        /// Copies `values` in, starting at element `at`. Splits across blocks where it has to, so
        /// the caller never has to know where the boundaries fell. The room must have been reserved.
        template <class T>
        void writeAt(std::uint32_t at, std::span<const T> values)
        {
            assert(sizeof(T) == mStride);

            std::uint32_t written = 0;
            while (written < values.size())
            {
                const std::uint32_t element = at + written;
                const std::uint32_t room = mBlockSize - element % mBlockSize;
                const auto part = std::min<std::uint32_t>(room, static_cast<std::uint32_t>(values.size()) - written);

                assert(blockOf(element) < mBlocks.size());
                mBlocks[blockOf(element)].writeAt(offsetOf(element), values.subspan(written, part));
                written += part;
            }
        }

        /// Which block an element is in, and how far into it, in bytes.
        std::uint32_t blockOf(std::uint32_t element) const { return element / mBlockSize; }
        VkDeviceSize offsetOf(std::uint32_t element) const { return VkDeviceSize{ element % mBlockSize } * mStride; }

        /// Where an element sits on the device, for a builder that reads it directly.
        ///
        /// **A contract**: a scene reaching past what has been reserved is a caller that skipped a
        /// `reserve`, not a table that should quietly grow under a builder.
        VkDeviceAddress addressOf(std::uint32_t element) const
        {
            assert(blockOf(element) < mAddresses.size());
            return mAddresses[blockOf(element)] + offsetOf(element);
        }

        /// Where every block starts, as a shader reads it so it can resolve a global id itself.
        VkBuffer getTable() const { return mTable.getHandle(); }

        VkDeviceSize getBytes() const { return getBlockBytes() * mBlocks.size(); }

    private:
        const Device* mDevice = nullptr;
        VkBufferUsageFlags mUsage = 0;
        std::string mName;

        std::uint32_t mBlockSize;
        std::uint32_t mStride;

        std::vector<HostBuffer> mBlocks;

        /// Kept beside the blocks rather than asked for, because a device address is a driver call
        /// and a table of them goes to the device on every growth.
        std::vector<VkDeviceAddress> mAddresses;
        HostBuffer mTable;
    };
}
