#pragma once

#include <cstdint>

#include <osg/Vec3f>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/camera.h>
#include <components/rtx/spritelistsize.hpp>

#include "buffer.hpp"

namespace Rtx
{
    class Device;
    class GpuTimer;
    class Graveyard;
    class SpriteBinPass;
    class SpriteShadePass;

    /// What a placement holds of its sprites, for a bin to copy and shade: the pristine table the
    /// host wrote and the emitters that placed them.
    struct SpriteSource
    {
        const Buffer* mSprites = nullptr;
        VkDeviceAddress mEmitters = 0;
        std::uint32_t mSpriteCount = 0;
        std::uint32_t mEmitterCount = 0;
    };

    /// The sprite tables a trace makes for itself: the sprites shaded against its sun, the depth
    /// order and the rectangles the passes scratch in, the screen tiles' list the trace reads, and
    /// the report of how long that list needed to be. **The trace's and not the placement's**,
    /// because every one of them is a function of a camera and a sun — and the placement copy they
    /// lived in was read by a frame in flight while the next trace's bin rewrote the sprites in
    /// place from the host. One per frame in flight in the world's chain, and one for the pictures,
    /// whose batches are ordered on the queue and touch nothing from the host.
    class SpriteBin
    {
    public:
        SpriteBin(const Device& device, Graveyard& graveyard);

        /// Copies `source`'s sprites into this bin's own table, shades them against `toSun` in
        /// place, and records the bin of them into the screen tiles of `camera` — ahead of the
        /// trace that reads the tiles, in the same commands. Grows the list first from what the
        /// last bin here reported it needed, where the timeline says that report has landed.
        void record(const SpriteShadePass& shading, const SpriteBinPass& pass, const SpriteSource& source,
            const osg::Vec3f& origin, const Shaders::Camera& camera, const osg::Vec3f& toSun, VkCommandBuffer commands,
            GpuTimer* timer);

        VkDeviceAddress getSpritesAddress() const { return mSprites.addressFor(); }
        VkDeviceAddress getTileListAddress() const { return mTileList.addressFor(); }

        VkDeviceSize getBytes() const;

    private:
        const Device& mDevice;

        /// Where a list this outgrows goes, until the frame reading it has run.
        Graveyard& mGraveyard;

        /// The shaded sprites, copied from the placement's before every shade because the shade
        /// writes over what it reads.
        Buffer mSprites;

        /// One depth key per sprite per light, the shading's own scratch inside its dispatch.
        /// `Shaders::SpriteShadeConstants::mOrder` says how the two lights share it.
        Buffer mOrder;

        /// One rectangle of tiles per sprite, the bin's own scratch between its dispatches.
        Buffer mRects;

        /// The sprite tiles' list, made on the device by `SpriteBinPass` and never written by the
        /// host: `tiles + 1` starts, then the runs, in `RunList`'s shape.
        Buffer mTileList;

        /// How many entries the last bin here came to, written by the pass and read back before the
        /// next bin. Staging, because it is the one table the host reads.
        Buffer mReport;

        /// How long `mTileList` is and how much of it a bin may fill. Grown from the report and
        /// never shrunk, so the list settles at its high-water mark like every other table.
        /// `SpriteListSize` says why the two numbers are one object.
        SpriteListSize mListSize;
    };
}
