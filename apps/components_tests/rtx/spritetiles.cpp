#include <cmath>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/camera.hpp>
#include <components/rtx/spritetiles.hpp>

namespace
{
    using namespace Rtx;

    constexpr std::uint32_t sWidth = 64;
    constexpr std::uint32_t sHeight = 48;

    Shaders::VisibilityConstants lookingAlongX()
    {
        return makeCamera(osg::Vec3f(0.0f, 0.0f, 0.0f), osg::Vec3f(1.0f, 0.0f, 0.0f), 60.0f, sWidth, sHeight, 10000.0f);
    }

    /// The ray through one pixel, derived here rather than shared.
    ///
    /// **`rayAt` is shader-only, and a test that called it would be checking the binning against the
    /// same arithmetic it was built from.** This is the cross-check: the derivation is written out
    /// from what `camera.h` says a ray is, and the tiles have to hold every sprite it can reach.
    osg::Vec3f rayThrough(const Shaders::Camera& camera, std::uint32_t x, std::uint32_t y)
    {
        const float u
            = (static_cast<float>(x) + 0.5f + camera.mJitter.x()) / static_cast<float>(camera.mWidth) * 2.0f - 1.0f;
        const float v
            = (static_cast<float>(y) + 0.5f + camera.mJitter.y()) / static_cast<float>(camera.mHeight) * 2.0f - 1.0f;

        osg::Vec3f direction = camera.mForward + camera.mRight * u - camera.mUp * v;
        direction.normalize();

        return direction;
    }

    /// Whether the march would take this sprite on this ray — the disc a billboard is tested against,
    /// and the swung quad an oriented one is, both copied from `spritesAlong`.
    bool marchWouldMeet(const Shaders::GpuSprite& sprite, const Shaders::GpuEmitter& emitter, const osg::Vec3f& origin,
        const osg::Vec3f& direction)
    {
        const osg::Vec3f toSprite = sprite.mPosition - origin;
        const bool oriented = emitter.mAcross.length2() > 0.0f && emitter.mUpward.length2() > 0.0f;

        if (!oriented)
        {
            const float depth = toSprite * direction;
            if (depth <= 0.0f)
                return false;

            return (toSprite - direction * depth).length() < sprite.mRadius;
        }

        const osg::Vec3f axis = emitter.mUpward;
        const osg::Vec3f swung = axis ^ direction;
        const float swing = swung.length();
        const osg::Vec3f side = swing > 1.0e-4f ? swung * (emitter.mAcross.length() / swing) : emitter.mAcross;

        const osg::Vec3f across = side * sprite.mRadius;
        const osg::Vec3f upward = axis * sprite.mRadius;
        const osg::Vec3f normal = across ^ upward;

        const float facing = normal * direction;
        if (std::abs(facing) <= 1.0e-6f)
            return false;

        const float depth = (toSprite * normal) / facing;
        if (depth <= 0.0f)
            return false;

        const osg::Vec3f offset = direction * depth - toSprite;

        return std::abs((offset * across) / (across * across)) < 1.0f
            && std::abs((offset * upward) / (upward * upward)) < 1.0f;
    }

    /// Whether `sprite` is among what `tiles` bins into the tile `(x, y)` falls in.
    bool binnedFor(const SpriteTiles& tiles, std::uint32_t sprite, std::uint32_t x, std::uint32_t y)
    {
        const std::size_t tile = std::size_t{ y / Shaders::SPRITE_TILE } * tiles.getAcross() + x / Shaders::SPRITE_TILE;

        for (std::uint32_t at = tiles.getOffsets()[tile]; at < tiles.getOffsets()[tile + 1]; ++at)
            if (tiles.getIndices()[at] == sprite)
                return true;

        return false;
    }

    /// A billboard emitter and an oriented one, and the sprites they hold.
    struct Layer
    {
        std::vector<Shaders::GpuSprite> mSprites;
        std::vector<Shaders::GpuEmitter> mEmitters;

        void addEmitter(const osg::Vec3f& across, const osg::Vec3f& upward)
        {
            Shaders::GpuEmitter emitter{};
            emitter.mFirst = static_cast<std::uint32_t>(mSprites.size());
            emitter.mCount = 0;
            emitter.mAcross = across;
            emitter.mUpward = upward;
            mEmitters.push_back(emitter);
        }

        void addSprite(const osg::Vec3f& position, float radius)
        {
            Shaders::GpuSprite sprite{};
            sprite.mPosition = position;
            sprite.mRadius = radius;
            sprite.mEmitter = static_cast<std::uint32_t>(mEmitters.size() - 1);
            mSprites.push_back(sprite);
            ++mEmitters.back().mCount;
        }
    };

    /// **The whole property, checked against the march itself.** A tile's list has to hold every
    /// sprite any ray through it can meet, because the shader's own test is a refinement and never a
    /// correction — a sprite the binning drops is a raindrop that stops being drawn.
    ///
    /// Every pixel of a small frame, against every sprite, both kinds of quad and a jittered camera.
    TEST(RtxSpriteTilesTest, aTileHoldsEverySpriteAnyRayThroughItCanMeet)
    {
        Layer layer;

        // Billboards spread across the view and in depth, including one that grazes the edge.
        layer.addEmitter(osg::Vec3f(), osg::Vec3f());
        for (float x : { 20.0f, 60.0f, 200.0f })
            for (float y : { -30.0f, 0.0f, 17.0f })
                for (float z : { -12.0f, 0.0f, 9.0f })
                    layer.addSprite(osg::Vec3f(x, y, z), 4.0f);

        // A rain streak: a tenth as wide as it is long, falling straight down.
        layer.addEmitter(osg::Vec3f(0.1f, 0.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, -1.0f));
        for (float x : { 30.0f, 90.0f })
            for (float y : { -20.0f, 5.0f, 25.0f })
                layer.addSprite(osg::Vec3f(x, y, 3.0f), 6.0f);

        for (const osg::Vec2f jitter : { osg::Vec2f(0.0f, 0.0f), osg::Vec2f(0.49f, -0.49f) })
        {
            Shaders::VisibilityConstants constants = lookingAlongX();
            constants.mCamera.mJitter = jitter;

            SpriteTiles tiles;
            tiles.rebuild(layer.mSprites, layer.mEmitters, constants.mOrigin, constants.mCamera);

            ASSERT_EQ(tiles.getAcross(), (sWidth + Shaders::SPRITE_TILE - 1) / Shaders::SPRITE_TILE);
            ASSERT_EQ(tiles.getDown(), (sHeight + Shaders::SPRITE_TILE - 1) / Shaders::SPRITE_TILE);

            std::uint32_t met = 0;
            for (std::uint32_t y = 0; y < sHeight; ++y)
                for (std::uint32_t x = 0; x < sWidth; ++x)
                {
                    const osg::Vec3f direction = rayThrough(constants.mCamera, x, y);

                    for (std::uint32_t at = 0; at < layer.mSprites.size(); ++at)
                    {
                        if (!marchWouldMeet(layer.mSprites[at], layer.mEmitters[layer.mSprites[at].mEmitter],
                                constants.mOrigin, direction))
                            continue;

                        ++met;
                        EXPECT_TRUE(binnedFor(tiles, at, x, y))
                            << "sprite " << at << " met at pixel " << x << ", " << y;
                    }
                }

            // A property nothing meets is a property nothing checks.
            EXPECT_GT(met, 200u) << "the fixture stopped covering the frame";
        }
    }

    /// A tile's run ascends, because that is the order the march composites in.
    ///
    /// Sprites blend in the order they are walked, and the loop this replaced walked emitters in
    /// order and indices within one. Sprite indices are contiguous per emitter, so ascending index
    /// is that same order — which is what lets a byte comparison check the change at all.
    TEST(RtxSpriteTilesTest, aTilesRunAscendsSoTheCompositeOrderIsTheOneTheMarchKept)
    {
        Layer layer;
        layer.addEmitter(osg::Vec3f(), osg::Vec3f());
        for (float y : { -8.0f, -4.0f, 0.0f, 4.0f, 8.0f })
            layer.addSprite(osg::Vec3f(40.0f, y, 0.0f), 30.0f);

        const Shaders::VisibilityConstants constants = lookingAlongX();
        SpriteTiles tiles;
        tiles.rebuild(layer.mSprites, layer.mEmitters, constants.mOrigin, constants.mCamera);

        std::uint32_t runs = 0;
        for (std::size_t tile = 0; tile + 1 < tiles.getOffsets().size(); ++tile)
        {
            const std::uint32_t from = tiles.getOffsets()[tile];
            const std::uint32_t to = tiles.getOffsets()[tile + 1];
            if (to - from < 2)
                continue;

            ++runs;
            for (std::uint32_t at = from + 1; at < to; ++at)
                EXPECT_LT(tiles.getIndices()[at - 1], tiles.getIndices()[at]) << "tile " << tile;
        }

        EXPECT_GT(runs, 0u) << "no tile held more than one sprite, so nothing was ordered";
    }

    /// A sprite the eye is inside covers whatever it likes, and there are no tangent lines to work
    /// that out from — so it goes in every tile rather than being reasoned about.
    TEST(RtxSpriteTilesTest, aSpriteAroundTheEyeIsInEveryTile)
    {
        Layer layer;
        layer.addEmitter(osg::Vec3f(), osg::Vec3f());
        layer.addSprite(osg::Vec3f(1.0f, 0.0f, 0.0f), 50.0f);

        const Shaders::VisibilityConstants constants = lookingAlongX();
        SpriteTiles tiles;
        tiles.rebuild(layer.mSprites, layer.mEmitters, constants.mOrigin, constants.mCamera);

        const std::size_t tileCount = std::size_t{ tiles.getAcross() } * tiles.getDown();
        ASSERT_EQ(tiles.getIndices().size(), tileCount);
        for (std::size_t tile = 0; tile < tileCount; ++tile)
            EXPECT_EQ(tiles.getOffsets()[tile + 1] - tiles.getOffsets()[tile], 1u) << "tile " << tile;
    }

    /// A sprite behind the eye reaches no tile, and one off to the side reaches only its own.
    TEST(RtxSpriteTilesTest, whatTheFrameCannotSeeIsBinnedNowhereAndTheRestIsBinnedNarrowly)
    {
        Layer layer;
        layer.addEmitter(osg::Vec3f(), osg::Vec3f());
        layer.addSprite(osg::Vec3f(-200.0f, 0.0f, 0.0f), 4.0f);
        layer.addSprite(osg::Vec3f(100.0f, 0.0f, 0.0f), 2.0f);

        const Shaders::VisibilityConstants constants = lookingAlongX();
        SpriteTiles tiles;
        tiles.rebuild(layer.mSprites, layer.mEmitters, constants.mOrigin, constants.mCamera);

        for (const std::uint32_t index : tiles.getIndices())
            EXPECT_EQ(index, 1u) << "the sprite behind the eye reached a tile";

        // Two units across at a hundred away is under a pixel of a sixty-degree frame, so the slack
        // the jitter needs is the whole of what it covers: four tiles at the very most.
        EXPECT_GT(tiles.getIndices().size(), 0u);
        EXPECT_LE(tiles.getIndices().size(), 4u);
    }

    /// The orthographic camera slides the eye instead of turning the ray, so a sprite's tiles are
    /// where it stands rather than where it points.
    TEST(RtxSpriteTilesTest, theOrthographicCameraBinsWhereTheSpriteStands)
    {
        Layer layer;
        layer.addEmitter(osg::Vec3f(), osg::Vec3f());
        layer.addSprite(osg::Vec3f(50.0f, 0.0f, 0.0f), 2.0f);

        osg::Matrixf view;
        view.makeLookAt(osg::Vec3f(0.0f, 0.0f, 0.0f), osg::Vec3f(1.0f, 0.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 1.0f));
        const Shaders::VisibilityConstants constants
            = makeOrthographicCameraFromView(view, 128.0f, 96.0f, sWidth, sHeight, 1.0f, 10000.0f);

        SpriteTiles tiles;
        tiles.rebuild(layer.mSprites, layer.mEmitters, constants.mOrigin, constants.mCamera);

        // Dead centre of the box, so the middle tiles hold it and the corners do not.
        for (std::uint32_t y = 0; y < sHeight; ++y)
            for (std::uint32_t x = 0; x < sWidth; ++x)
            {
                const osg::Vec3f offset = constants.mCamera.mRight
                        * ((static_cast<float>(x) + 0.5f) / static_cast<float>(sWidth) * 2.0f - 1.0f)
                    - constants.mCamera.mUp
                        * ((static_cast<float>(y) + 0.5f) / static_cast<float>(sHeight) * 2.0f - 1.0f);
                const osg::Vec3f from = constants.mOrigin + offset;

                osg::Vec3f along = constants.mCamera.mForward;
                along.normalize();

                const osg::Vec3f toSprite = layer.mSprites[0].mPosition - from;
                const float depth = toSprite * along;
                if (depth <= 0.0f || (toSprite - along * depth).length() >= layer.mSprites[0].mRadius)
                    continue;

                EXPECT_TRUE(binnedFor(tiles, 0, x, y)) << "pixel " << x << ", " << y;
            }

        EXPECT_GT(tiles.getIndices().size(), 0u);
    }
}
