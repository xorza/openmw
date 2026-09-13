#include "spritebin.hpp"

#include <components/rtx/shaders/spritebin.h>
#include <components/rtx/shaders/spriteshade.h>

#include "device.hpp"
#include "gputimer.hpp"
#include "graveyard.hpp"
#include "spritepasses.hpp"
#include "timeline.hpp"

namespace Rtx
{
    namespace
    {
        constexpr VkBufferUsageFlags sTableUsage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

        /// Copied into from the placement's table before every shade.
        constexpr VkBufferUsageFlags sSpriteUsage = sTableUsage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        /// The one table the device fills, and its head is zeroed by a fill before every bin.
        constexpr VkBufferUsageFlags sListUsage = sTableUsage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }

    SpriteBin::SpriteBin(const Device& device)
        : mDevice(device)
        , mSprites(Buffer::hostWritten(device, 1, sSpriteUsage))
        , mOrder(Buffer::hostWritten(device, 1, sTableUsage))
        , mRects(Buffer::hostWritten(device, 1, sTableUsage))
        , mTileList(Buffer::hostWritten(device, 1, sListUsage))
        , mReport(Buffer::staging(device, sizeof(std::uint32_t), sTableUsage))
    {
        // Every table exists from here, whether or not anything is binned: a frame carries the
        // address of the sprites and the list, and a scene with no sprites bins none.
        *static_cast<std::uint32_t*>(mReport.map()) = 0;
    }

    void SpriteBin::record(const SpriteShadePass& shading, const SpriteBinPass& pass, const SpriteSource& source,
        const osg::Vec3f& origin, const Shaders::Camera& camera, const osg::Vec3f& toSun, VkCommandBuffer commands,
        GpuTimer* const timer, Graveyard& graveyard)
    {
        const Timeline& timeline = mDevice.getTimeline();
        const std::uint32_t count = source.mSpriteCount;
        const VkDeviceSize bytes = source.mSprites->getSize();

        graveyard.bury(growTo(mSprites, mDevice, bytes, sSpriteUsage));
        graveyard.bury(growTo(mOrder, mDevice,
            VkDeviceSize{ count } * Shaders::SPRITE_SHADE_LIGHTS * sizeof(std::uint64_t), sTableUsage));
        graveyard.bury(growTo(mRects, mDevice, VkDeviceSize{ count } * sizeof(std::uint64_t), sTableUsage));

        // The placement's table, whole, because the shade writes over what it reads: the copy is
        // what lets a second trace against the same placement — a picture, or the frame after a
        // picture — start from sprites nothing has shaded. On the queue and not from the host,
        // so nothing a submit in flight reads is written under it. Whoever read this bin's tables
        // last is behind on the queue, and the barrier `CommandPool::begin` records at the head
        // of these commands is what orders the copy after it.
        const VkBufferCopy region{ .size = bytes };
        vkCmdCopyBuffer(commands, source.mSprites->getHandle(), mSprites.getHandle(), 1, &region);

        const VkMemoryBarrier2 copied{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        };
        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &copied,
        };
        vkCmdPipelineBarrier2(commands, &dependency);

        shading.record(commands,
            Shaders::SpriteShadeConstants{
                .mSprites = mSprites.getDeviceAddress(),
                .mEmitters = source.mEmitters,
                .mOrder = mOrder.getDeviceAddress(),
                .mToSun = toSun,
                .mEmitterCount = source.mEmitterCount,
                .mCount = count,
            },
            timer);

        // Sized from what the last bin here said it needed, with room over it, because the need is
        // only known once the tiles are counted and that happens on the device. Read where the
        // timeline says the report has landed; a picture's bin behind another's on the queue keeps
        // the capacity it has, which `SpriteListSize` never shrinks anyway.
        const std::uint32_t reported
            = timeline.hasFinished(mReport) ? *static_cast<const std::uint32_t*>(mReport.map()) : 0;
        mListSize.sizeFor(Shaders::spriteTilesIn(camera.mWidth, camera.mHeight), count, reported);

        graveyard.bury(growTo(mTileList, mDevice, mListSize.getBytes(), sListUsage));

        pass.record(commands,
            Shaders::SpriteBinConstants{
                .mSprites = mSprites.getDeviceAddress(),
                .mEmitters = source.mEmitters,
                .mRects = mRects.getDeviceAddress(),
                .mList = mTileList.getDeviceAddress(),
                .mReport = mReport.getDeviceAddress(),
                .mOrigin = origin,
                .mCamera = camera,
                .mCount = count,
                .mCapacity = mListSize.getCapacity(),
            },
            mTileList, timer);

        // What the next bin here sizes its list from. A fence's access scope is the device's, so
        // without this the figure is whatever the caches held.
        mReport.orderForHostRead(commands);
        mReport.nameFor(timeline.getNext());
    }

    VkDeviceSize SpriteBin::getBytes() const
    {
        return mSprites.getSize() + mOrder.getSize() + mRects.getSize() + mTileList.getSize() + mReport.getSize();
    }
}
