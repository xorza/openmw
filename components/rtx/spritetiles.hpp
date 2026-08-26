#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <osg/Vec3f>

#include "shaders/camera.h"
#include "shaders/scene.h"

namespace Rtx
{
    /// Which sprites each tile of the screen can see, as two arrays.
    ///
    /// **The emitter's sphere is the wrong granularity, and rain is what proves it.** A brazier is a
    /// point: the ray-sphere test in `spritesAlong` throws its whole system away for almost every
    /// pixel, which is exactly what it was written for. A rainstorm is one emitter whose sphere is
    /// the entire view, so every pixel is admitted and then walks all of its sprites — two thousand
    /// nine hundred of them, 2.7 billion tests a frame, and ninety-six per cent of the trace.
    /// Binning the *emitters* changes none of that because no tile rejects the rain. Binning the
    /// sprites does, because a raindrop is small on the screen wherever it is in the world.
    ///
    /// Tile `i` owns `getIndices()[o[i] .. o[i + 1]]` with `o` the offsets: a prefix sum with a
    /// trailing sentinel, so the whole structure is two arrays and a lookup is two reads — the shape
    /// `LightGrid` has, over a different thing.
    ///
    /// **Ascending sprite index within a tile, and that is load-bearing.** Sprites composite in the
    /// order they are walked and the current loop walks emitters in order and indices within one; a
    /// sprite's index is contiguous per emitter, so ascending index reproduces that order exactly.
    /// It also keeps an emitter's sprites consecutive, which is what lets the shader evaluate the
    /// fog once for a run rather than once for a sprite.
    ///
    /// **Screen space, and so rebuilt every frame against the camera.** Unlike the light grid, which
    /// a reflection or a bounce can look into from anywhere, the sprite layer is marched against the
    /// primary ray and nothing else — `spritesAlong` is called once, from `main`, with the pixel's
    /// own ray.
    class SpriteTiles
    {
    public:
        /// Bins `sprites` into the buffers this already has.
        ///
        /// Conservative by construction: a tile's list holds every sprite any ray through it could
        /// meet, so the shader's own ray-quad test stays a refinement and never a correction.
        ///
        /// @param emitters what the sprites belong to, for the axes an oriented quad hangs on.
        ///        `GpuSprite::mEmitter` is what indexes this.
        void rebuild(std::span<const Shaders::GpuSprite> sprites, std::span<const Shaders::GpuEmitter> emitters,
            const osg::Vec3f& origin, const Shaders::Camera& camera);

        std::uint32_t getAcross() const { return mAcross; }
        std::uint32_t getDown() const { return mDown; }

        /// `getAcross() * getDown() + 1`: every tile's start, and a sentinel so the last tile's end
        /// needs no special case.
        std::span<const std::uint32_t> getOffsets() const { return mOffsets; }

        /// Every tile's sprites, run together in tile order and ascending inside each run.
        std::span<const std::uint32_t> getIndices() const { return mIndices; }

    private:
        std::uint32_t mAcross = 0;
        std::uint32_t mDown = 0;
        std::vector<std::uint32_t> mOffsets;
        std::vector<std::uint32_t> mIndices;

        /// Where each tile's next entry goes while the runs are being filled. A member for the
        /// reason the two above are refilled rather than replaced: a frame must not go back to the
        /// allocator for a table it already has.
        std::vector<std::uint32_t> mCursor;
    };
}
