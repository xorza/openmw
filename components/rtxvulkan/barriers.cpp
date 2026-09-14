#include "barriers.hpp"

#include <cstdint>

namespace Rtx
{
    void Barriers::add(const VkImageMemoryBarrier2& barrier)
    {
        if (mImageCount == mImages.size())
            flush();

        mImages[mImageCount++] = barrier;
    }

    void Barriers::add(const VkBufferMemoryBarrier2& barrier)
    {
        if (mBufferCount == mBuffers.size())
            flush();

        mBuffers[mBufferCount++] = barrier;
    }

    void Barriers::add(const VkMemoryBarrier2& barrier)
    {
        if (mMemoryCount == mMemory.size())
            flush();

        mMemory[mMemoryCount++] = barrier;
    }

    void Barriers::flush()
    {
        if (mImageCount == 0 && mBufferCount == 0 && mMemoryCount == 0)
            return;

        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = static_cast<std::uint32_t>(mMemoryCount),
            .pMemoryBarriers = mMemory.data(),
            .bufferMemoryBarrierCount = static_cast<std::uint32_t>(mBufferCount),
            .pBufferMemoryBarriers = mBuffers.data(),
            .imageMemoryBarrierCount = static_cast<std::uint32_t>(mImageCount),
            .pImageMemoryBarriers = mImages.data(),
        };
        vkCmdPipelineBarrier2(mCommands, &dependency);

        mImageCount = 0;
        mBufferCount = 0;
        mMemoryCount = 0;
    }
}
