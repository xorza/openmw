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
        /// `capsuleTiles` is what guarantees.
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

        /// Where something lands on the screen, in whole pixels, before the frame's edges are applied.
        ///
        /// **Off the frame and not clipped to it**, because a capsule is bounded by two of these and
        /// a rectangle clipped to nothing carries no side to union against: a streak lying across the
        /// view has both its ends past the edge and its middle down the centre of the frame.
        struct PixelRect
        {
            std::int32_t mFromX = 0;
            std::int32_t mFromY = 0;
            std::int32_t mToX = 0;
            std::int32_t mToY = 0;

            void add(const PixelRect& other)
            {
                mFromX = std::min(mFromX, other.mFromX);
                mFromY = std::min(mFromY, other.mFromY);
                mToX = std::max(mToX, other.mToX);
                mToY = std::max(mToY, other.mToY);
            }
        };

        /// Where a ball of `radius` about `centre` lands, given that its tangent cone is bounded —
        /// which for a perspective frame is `depth > radius`, and for an orthographic one is always.
        PixelRect ballPixels(const osg::Vec3f& centre, float radius, const osg::Vec3f& origin, const CameraFrame& frame,
            const Shaders::Camera& camera)
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

                const SlopeSpan sideways = tangentSlopes(toward * frame.mRight, depth, radius);
                const SlopeSpan upright = tangentSlopes(-(toward * frame.mUp), depth, radius);

                lowU = sideways.mLow / frame.mHalfWidth;
                highU = sideways.mHigh / frame.mHalfWidth;
                lowV = upright.mLow / frame.mHalfHeight;
                highV = upright.mHigh / frame.mHalfHeight;
            }

            return PixelRect{
                toPixel(lowU, camera.mWidth, -1.0f),
                toPixel(lowV, camera.mHeight, -1.0f),
                toPixel(highU, camera.mWidth, 1.0f),
                toPixel(highV, camera.mHeight, 1.0f),
            };
        }

        /// The tiles a pixel rectangle covers, or nothing where it misses the frame.
        TileRect tilesOf(const PixelRect& rect, const Shaders::Camera& camera)
        {
            if (rect.mToX < 0 || rect.mToY < 0 || rect.mFromX >= static_cast<std::int32_t>(camera.mWidth)
                || rect.mFromY >= static_cast<std::int32_t>(camera.mHeight))
                return TileRect{};

            const auto tile = [](std::int32_t pixel, std::uint32_t limit) {
                return std::clamp(pixel, 0, static_cast<std::int32_t>(limit) - 1) / Shaders::SPRITE_TILE;
            };

            return TileRect{
                static_cast<std::uint32_t>(tile(rect.mFromX, camera.mWidth)),
                static_cast<std::uint32_t>(tile(rect.mFromY, camera.mHeight)),
                static_cast<std::uint32_t>(tile(rect.mToX, camera.mWidth)),
                static_cast<std::uint32_t>(tile(rect.mToY, camera.mHeight)),
            };
        }

        /// Everywhere a sprite's own drawing can reach, as a segment and a radius about it.
        ///
        /// **A ball for a billboard and a capsule for an oriented quad**, because that is the shape
        /// the march tests against. A billboard is rejected past `mRadius` of the ray, so the ball
        /// is exact. An oriented quad hangs on its authored axis and swings its width about that
        /// axis to meet the ray — so over every direction the eye can look from, it sweeps a
        /// cylinder of the width's radius about the axis segment, and the capsule is that cylinder
        /// with its ends rounded.
        ///
        /// **A sphere around the corners is what this replaces, and rain is what it cost.** A drop
        /// is a long thin streak, so the sphere is as wide as the streak is long — tens of times the
        /// width — and a bound that wide crosses the eye plane for every drop falling anywhere near
        /// the camera. There are no tangent lines from inside, so each of those was binned into
        /// every tile on the screen: ninety-four drops out of two thousand six hundred, and 96% of
        /// the whole index table. The capsule's radius is the streak's width instead of its length,
        /// which is what takes that case back to almost never.
        struct SpriteBound
        {
            osg::Vec3f mFrom;
            osg::Vec3f mTo;
            float mRadius = 0.0f;
        };

        SpriteBound boundOf(const Shaders::GpuSprite& sprite, const Shaders::GpuEmitter& emitter)
        {
            const float across = emitter.mAcross.length();
            const float upward = emitter.mUpward.length();
            if (across <= 0.0f || upward <= 0.0f)
                return SpriteBound{ sprite.mPosition, sprite.mPosition, sprite.mRadius };

            const osg::Vec3f half = emitter.mUpward * sprite.mRadius;

            return SpriteBound{ sprite.mPosition - half, sprite.mPosition + half, sprite.mRadius * across };
        }

        /// The tiles a capsule can be met from, as the rectangle around the two balls that cap it.
        ///
        /// **The rectangle around both and not a ball around both**, which is the whole of what a
        /// capsule buys: a streak lying across the frame covers a long thin rectangle, and a ball
        /// holding the same two ends covers the square that rectangle sits in. Both caps in front of
        /// the eye, the segment between them projects to a straight line between their two centres,
        /// so a rectangle holding the caps holds the cylinder as well.
        TileRect capsuleTiles(const SpriteBound& bound, const osg::Vec3f& origin, const CameraFrame& frame,
            const Shaders::Camera& camera, std::uint32_t across, std::uint32_t down)
        {
            if (camera.mOrthographic == 0u)
            {
                const float from = (bound.mFrom - origin) * frame.mForward;
                const float to = (bound.mTo - origin) * frame.mForward;

                // Wholly behind the eye, where no ray of a frame that only looks forward can reach
                // it. The march's own depth test says the same, so this is a tile saved rather than
                // a sprite lost.
                if (from <= -bound.mRadius && to <= -bound.mRadius)
                    return TileRect{};

                // **A cap the eye is inside, or level with.** There are no tangent lines from a
                // point on or inside a circle, and neither is there a straight projected segment
                // once the capsule reaches the plane through the eye — so it goes in every tile
                // rather than being reasoned about.
                if (from <= bound.mRadius || to <= bound.mRadius)
                    return TileRect{ 0, 0, across - 1, down - 1 };
            }

            PixelRect rect = ballPixels(bound.mFrom, bound.mRadius, origin, frame, camera);
            if (bound.mTo != bound.mFrom)
                rect.add(ballPixels(bound.mTo, bound.mRadius, origin, frame, camera));

            return tilesOf(rect, camera);
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
                const TileRect rect = capsuleTiles(boundOf(sprite, emitter), origin, frame, camera, mAcross, mDown);
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
