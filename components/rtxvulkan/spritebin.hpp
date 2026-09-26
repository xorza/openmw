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

    /// What one bin is of: the sprites, where they are seen from, and what lights them. A record
    /// and not an argument list, because `mOrigin` is a place and `mToSun` a direction and the
    /// two are one type, so a list takes either for the other.
    struct Binning
    {
        const SpriteShadePass& mShading;
        const SpriteBinPass& mPass;

        /// The sprites this bin took, as the scene's tables describe them.
        const SpriteSource& mSource;

        /// Where the eye stands, and the camera whose screen tiles the sprites are binned into.
        osg::Vec3f mOrigin;
        Shaders::Camera mCamera;

        /// Toward the sun, which the shade lights every sprite by.
        osg::Vec3f mToSun;

        /// Null where the run is not being timed.
        GpuTimer* mTimer = nullptr;
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
        explicit SpriteBin(const Device& device);

        /// Grows every table here to what `source` and the tiles of `camera` need — the list from
        /// what the last bin here reported it needed, where the timeline says that report has
        /// landed — and copies `source`'s sprites into this bin's own, on the queue, left where a
        /// launch or a dispatch may read and write them. First, and apart from `record`, because
        /// what stands between the two is the frame block: `getSpritesAddress` and
        /// `getTileListAddress` are only the addresses once the tables are grown, the block carries
        /// both, and the shelter launch that zeroes the sheltered sprites reads the block before
        /// the shade reads the sprites.
        void take(const SpriteSource& source, const Shaders::Camera& camera, VkCommandBuffer commands);

        /// Shades the sprites `take` copied against the sun in place, and records the bin of them
        /// into the camera's screen tiles — ahead of the trace that reads the tiles, in the same
        /// commands. `commands` first, as every other `record` in this backend takes it.
        void record(VkCommandBuffer commands, const Binning& what);

        VkDeviceAddress getSpritesAddress() const { return mSprites.addressFor(); }
        VkDeviceAddress getTileListAddress() const { return mTileList.addressFor(); }

        VkDeviceSize getBytes() const;

    private:
        const Device& mDevice;

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
