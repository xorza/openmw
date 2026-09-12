#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "buffer.hpp"

namespace Rtx
{
    class Batch;
    class Device;

    /// One table of fixed-size elements, kept as a list of separate buffers of a fixed count each,
    /// so a cell arriving adds a block and every address already built into an acceleration
    /// structure stays good. Full blocks even where the scene stops part way, or the last would be
    /// made again on the next arrival. Device memory, staged through the batch a load is already
    /// recording, because the memory the host writes into directly is a couple of hundred
    /// megabytes on a card without resizable BAR. `Rtx::SceneDesc` never lets a mesh's run
    /// straddle a block, so `addressOf` on a run's first element covers the whole run.
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
        std::uint32_t getStride() const { return mStride; }

        /// Bytes one whole block occupies, which is what every block is made at.
        VkDeviceSize getBlockBytes() const { return VkDeviceSize{ mBlockSize } * mStride; }

        /// Makes blocks until the table can hold `elements`, and rewrites the address table where it
        /// made any. Nothing already in it moves. At least one block always exists, because a table
        /// nothing has been put in still has to be bound.
        void reserve(Batch& batch, std::uint32_t elements);

        /// Copies `values` in, starting at element `at`. Splits across blocks where it has to, so
        /// the caller never has to know where the boundaries fell. The room must have been reserved.
        template <class T>
        void writeAt(Batch& batch, std::uint32_t at, std::span<const T> values)
        {
            assert(sizeof(T) == mStride);

            std::uint32_t written = 0;
            while (written < values.size())
            {
                const std::uint32_t element = at + written;
                const std::uint32_t room = mBlockSize - element % mBlockSize;
                const auto part = std::min<std::uint32_t>(room, static_cast<std::uint32_t>(values.size()) - written);

                writeInto(batch, element, std::as_bytes(values.subspan(written, part)));
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

        /// Where every block starts, as a shader reads it so it can resolve a global id itself: the
        /// address of the table of addresses, which the frame block carries.
        VkDeviceAddress getTableAddress() const { return mTable.getDeviceAddress(); }

        /// One block by number, for a test that copies what a kernel wrote into it back out.
        const Buffer& getBlock(std::uint32_t block) const
        {
            assert(block < mBlocks.size());
            return mBlocks[block];
        }

        VkDeviceSize getBytes() const { return getBlockBytes() * mBlocks.size(); }

    private:
        /// One run, staged and copied into the block it falls in. `writeAt` is what splits a run
        /// that straddles two.
        void writeInto(Batch& batch, std::uint32_t element, std::span<const std::byte> bytes);

        const Device* mDevice = nullptr;
        VkBufferUsageFlags mUsage = 0;
        std::string mName;

        std::uint32_t mBlockSize;
        std::uint32_t mStride;

        std::vector<Buffer> mBlocks;

        /// Kept beside the blocks as one run, because a table of them goes to the device on every
        /// growth.
        std::vector<VkDeviceAddress> mAddresses;
        Buffer mTable;
    };
}
