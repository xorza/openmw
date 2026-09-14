#pragma once

#include <array>
#include <cstddef>

#include <vulkan/vulkan_core.h>

#include "imageuse.hpp"

namespace Rtx
{
    /// A run of dependencies emitted as one `vkCmdPipelineBarrier2`: the G-buffer's fourteen
    /// channels change state together twice a frame, and a pass that clears a buffer and takes an
    /// image has one barrier to record and not two. No allocation, because this is a frame path; a
    /// batch that fills up emits what it holds and carries on.
    class Barriers
    {
    public:
        explicit Barriers(VkCommandBuffer commands)
            : mCommands(commands)
        {
        }

        void add(const VkImageMemoryBarrier2& barrier);
        void add(const VkBufferMemoryBarrier2& barrier);
        void add(const VkMemoryBarrier2& barrier);

        /// Emits what has been added, and empties. Does nothing where nothing was added.
        void flush();

    private:
        /// The longest runs this renderer has: the G-buffer's channels, which change state
        /// together twice a frame, and the exposure's two buffers. A run longer than this emits
        /// what it holds and carries on, so the figures bound the arrays rather than the caller.
        static constexpr std::size_t sMostImages = 16;
        static constexpr std::size_t sMostBuffers = 4;
        static constexpr std::size_t sMostMemory = 2;

        VkCommandBuffer mCommands = VK_NULL_HANDLE;
        std::array<VkImageMemoryBarrier2, sMostImages> mImages{};
        std::array<VkBufferMemoryBarrier2, sMostBuffers> mBuffers{};
        std::array<VkMemoryBarrier2, sMostMemory> mMemory{};
        std::size_t mImageCount = 0;
        std::size_t mBufferCount = 0;
        std::size_t mMemoryCount = 0;
    };

    /// Orders one pass's writes against what reads or writes them next: a memory barrier over
    /// everything, recorded on its own.
    inline void handOver(VkCommandBuffer commands, const BufferUse& from, const BufferUse& to)
    {
        Barriers barriers(commands);
        barriers.add(memoryBarrier(from, to));
        barriers.flush();
    }
}
