#include "spritetiles.hpp"

#include <algorithm>
#include <cmath>

namespace Rtx
{
    namespace
    {
        /// The tiles one sprite reaches, as an inclusive rectangle, or nothing where it reaches none.
        struct TileRect
        {
            std::uint32_t mFromX = 1;
            std::uint32_t mFromY = 1;
            std::uint32_t mToX = 0;
            std::uint32_t mToY = 0;

            bool isEmpty() const { return mToX < mFromX || mToY < mFromY; }
        };

        /// The camera's axes, unit, beside the half-extents they were scaled by.
        ///
        /// `makeCameraFromView` builds `mForward` unit and scales `mRight` and `mUp` by the image
        /// plane's half-width and half-height, so the three are mutually orthogonal and the two
        /// lengths are the only thing separating a direction from a screen coordinate.
        struct CameraFrame
        {
            osg::Vec3f mForward;
            osg::Vec3f mRight;
            osg::Vec3f mUp;
            float mHalfWidth = 1.0f;
            float mHalfHeight = 1.0f;
        };

        CameraFrame frameOf(const Shaders::Camera& camera)
        {
            CameraFrame frame{ camera.mForward, camera.mRight, camera.mUp };
            frame.mForward.normalize();
            frame.mHalfWidth = frame.mRight.normalize();
            frame.mHalfHeight = frame.mUp.normalize();

            return frame;
        }

        /// The two extreme slopes of the tangent lines from the eye to a circle at `(along, depth)`.
        ///
        /// The closed form rather than a projected bounding box, because a box around the sphere is
        /// three times the area at the edge of a wide frame, and every extra tile is a sprite walked
        /// by pixels that cannot see it. Only meaningful where the eye is outside the sphere, which
        /// `depth > radius` is what guarantees.
        struct SlopeSpan
        {
            float mLow = 0.0f;
            float mHigh = 0.0f;
        };

        SlopeSpan tangentSlopes(float along, float depth, float radius)
        {
            const float behind = depth * depth - radius * radius;
            const float reach = std::sqrt(std::max(along * along + behind, 0.0f));
            const float middle = along * depth;

            return SlopeSpan{ (middle - radius * reach) / behind, (middle + radius * reach) / behind };
        }

        /// Where a screen coordinate in minus-one-to-one lands, in whole pixels and generously.
        ///
        /// **A pixel of slack at each end, which is what covers the jitter.** `rayAt` adds half a
        /// pixel and the frame's jitter to the index before scaling, and the jitter is a Halton
        /// offset inside the pixel — so a coordinate is worth a pixel either way and the binning has
        /// to hold every ray the tile can produce, not the one through its centre.
        std::int32_t toPixel(float coordinate, std::uint32_t extent, float slack)
        {
            const float pixel = (coordinate + 1.0f) * 0.5f * static_cast<float>(extent) + slack;

            return static_cast<std::int32_t>(std::floor(pixel));
        }

        /// The tiles a sprite of `radius` about `centre` can be met from.
        TileRect tilesOf(const osg::Vec3f& centre, float radius, const osg::Vec3f& origin, const CameraFrame& frame,
            const Shaders::Camera& camera, std::uint32_t across, std::uint32_t down)
        {
            const osg::Vec3f toward = centre - origin;

            float lowU = 0.0f;
            float highU = 0.0f;
            float lowV = 0.0f;
            float highV = 0.0f;

            if (camera.mOrthographic != 0u)
            {
                // Every ray runs the same way and the eye slides across the plane instead, so the
                // sphere covers the coordinates whose ray passes within its radius. Behind the plane
                // is still binned: the march's own depth test is what rejects it, and this only has
                // to be no tighter than that.
                lowU = (toward * frame.mRight - radius) / frame.mHalfWidth;
                highU = (toward * frame.mRight + radius) / frame.mHalfWidth;

                // `rayAt` subtracts the up axis, so a sprite above the eye is at a smaller `v`.
                lowV = (-(toward * frame.mUp) - radius) / frame.mHalfHeight;
                highV = (-(toward * frame.mUp) + radius) / frame.mHalfHeight;
            }
            else
            {
                const float depth = toward * frame.mForward;

                // Wholly behind the eye, where no ray of a frame that only looks forward can reach
                // it. The march's own depth test says the same, so this is a tile saved rather than
                // a sprite lost.
                if (depth <= -radius)
                    return TileRect{};

                // **The eye is inside the sphere, or level with it.** There are no tangent lines
                // from a point on or inside a circle, and a sprite that close covers whatever it
                // likes — so it goes in every tile rather than being reasoned about.
                if (depth <= radius)
                    return TileRect{ 0, 0, across - 1, down - 1 };

                const SlopeSpan sideways = tangentSlopes(toward * frame.mRight, depth, radius);
                const SlopeSpan upright = tangentSlopes(-(toward * frame.mUp), depth, radius);

                lowU = sideways.mLow / frame.mHalfWidth;
                highU = sideways.mHigh / frame.mHalfWidth;
                lowV = upright.mLow / frame.mHalfHeight;
                highV = upright.mHigh / frame.mHalfHeight;
            }

            const std::int32_t fromX = toPixel(lowU, camera.mWidth, -1.0f);
            const std::int32_t toX = toPixel(highU, camera.mWidth, 1.0f);
            const std::int32_t fromY = toPixel(lowV, camera.mHeight, -1.0f);
            const std::int32_t toY = toPixel(highV, camera.mHeight, 1.0f);

            if (toX < 0 || toY < 0 || fromX >= static_cast<std::int32_t>(camera.mWidth)
                || fromY >= static_cast<std::int32_t>(camera.mHeight))
                return TileRect{};

            const auto tile = [](std::int32_t pixel, std::uint32_t limit) {
                return std::clamp(pixel, 0, static_cast<std::int32_t>(limit) - 1) / Shaders::SPRITE_TILE;
            };

            return TileRect{
                static_cast<std::uint32_t>(tile(fromX, camera.mWidth)),
                static_cast<std::uint32_t>(tile(fromY, camera.mHeight)),
                static_cast<std::uint32_t>(tile(toX, camera.mWidth)),
                static_cast<std::uint32_t>(tile(toY, camera.mHeight)),
            };
        }

        /// How far a sprite's own drawing reaches from its centre, in world units.
        ///
        /// **A disc for a billboard and a swung quad for an oriented one**, because that is what the
        /// march tests against: a billboard is rejected past `mRadius` of the ray, so the disc is
        /// exact; an oriented quad's corner is its two authored axes added, each scaled by the
        /// radius, and swinging the width about the axis cannot lengthen it.
        float reachOf(const Shaders::GpuSprite& sprite, const Shaders::GpuEmitter& emitter)
        {
            const float across = emitter.mAcross.length();
            const float upward = emitter.mUpward.length();
            if (across <= 0.0f || upward <= 0.0f)
                return sprite.mRadius;

            return sprite.mRadius * (across + upward);
        }
    }

    void SpriteTiles::rebuild(std::span<const Shaders::GpuSprite> sprites,
        std::span<const Shaders::GpuEmitter> emitters, const osg::Vec3f& origin, const Shaders::Camera& camera)
    {
        mAcross = (camera.mWidth + Shaders::SPRITE_TILE - 1) / Shaders::SPRITE_TILE;
        mDown = (camera.mHeight + Shaders::SPRITE_TILE - 1) / Shaders::SPRITE_TILE;

        const std::size_t tiles = std::size_t{ mAcross } * mDown;

        mOffsets.assign(tiles + 1, 0);
        mCursor.assign(tiles, 0);
        mIndices.clear();

        if (sprites.empty() || emitters.empty() || tiles == 0)
            return;

        const CameraFrame frame = frameOf(camera);

        // **Counted before it is filled**, so the runs are laid end to end with no room wasted and
        // no reallocation while a sprite is being placed. The rectangle is worked out twice rather
        // than kept, because keeping one per sprite is a table as long as the sprites and the
        // arithmetic is a dozen operations.
        const auto place = [&](auto&& visit) {
            for (std::uint32_t at = 0; at < sprites.size(); ++at)
            {
                const Shaders::GpuSprite& sprite = sprites[at];
                if (sprite.mEmitter >= emitters.size())
                    continue;

                const Shaders::GpuEmitter& emitter = emitters[sprite.mEmitter];
                const TileRect rect
                    = tilesOf(sprite.mPosition, reachOf(sprite, emitter), origin, frame, camera, mAcross, mDown);
                if (rect.isEmpty())
                    continue;

                for (std::uint32_t y = rect.mFromY; y <= rect.mToY; ++y)
                    for (std::uint32_t x = rect.mFromX; x <= rect.mToX; ++x)
                        visit(std::size_t{ y } * mAcross + x, at);
            }
        };

        place([&](std::size_t tile, std::uint32_t) { ++mOffsets[tile + 1]; });

        for (std::size_t tile = 0; tile < tiles; ++tile)
        {
            mOffsets[tile + 1] += mOffsets[tile];
            mCursor[tile] = mOffsets[tile];
        }

        mIndices.resize(mOffsets.back());

        // Ascending, because the sprites are walked ascending and each tile's cursor only moves
        // forward — which is the composite order the march already keeps.
        place([&](std::size_t tile, std::uint32_t sprite) { mIndices[mCursor[tile]++] = sprite; });
    }
}
