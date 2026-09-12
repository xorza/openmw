#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Vec3f>

#include <components/rtx/camera.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/shaders/spritebin.h>
#include <components/rtx/shaders/spriteshade.h>
#include <components/rtxvulkan/buffer.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/spritepasses.hpp>

#include "allocations.hpp"
#include "harness.hpp"

namespace Rtx
{
    namespace
    {
        constexpr std::uint32_t sWidth = 64;
        constexpr std::uint32_t sHeight = 48;

        /// Room for more entries than any fixture here makes, so only the test about room runs out.
        constexpr std::uint32_t sPlenty = 1u << 16;

        Shaders::VisibilityConstants lookingAlongX()
        {
            return makeCamera(
                osg::Vec3f(0.0f, 0.0f, 0.0f), osg::Vec3f(1.0f, 0.0f, 0.0f), 60.0f, sWidth, sHeight, 10000.0f);
        }

        /// The ray through one pixel, derived here rather than shared.
        ///
        /// **`rayAt` is shader-only, and a test that called it would be checking the binning against
        /// the same arithmetic it was built from.** This is the cross-check: the derivation is
        /// written out from what `camera.h` says a ray is, and the tiles have to hold every sprite it
        /// can reach.
        osg::Vec3f rayThrough(const Shaders::Camera& camera, std::uint32_t x, std::uint32_t y)
        {
            const float u
                = (static_cast<float>(x) + 0.5f + camera.mJitter.x()) / static_cast<float>(camera.mWidth) * 2.0f - 1.0f;
            const float v
                = (static_cast<float>(y) + 0.5f + camera.mJitter.y()) / static_cast<float>(camera.mHeight) * 2.0f
                - 1.0f;

            osg::Vec3f direction = camera.mForward + camera.mRight * u - camera.mUp * v;
            direction.normalize();

            return direction;
        }

        /// Whether the march would take this sprite on this ray — the disc a billboard is tested
        /// against, and the swung quad an oriented one is, both copied from `spritesAlong`.
        bool marchWouldMeet(const Shaders::GpuSprite& sprite, const Shaders::GpuEmitter& emitter,
            const osg::Vec3f& origin, const osg::Vec3f& direction)
        {
            const osg::Vec3f toSprite = sprite.mPosition - origin;

            if (!(emitter.mWidth > 0.0f))
            {
                const float depth = toSprite * direction;
                if (depth <= 0.0f)
                    return false;

                return (toSprite - direction * depth).length() < sprite.mRadius;
            }

            const osg::Vec3f axis = sprite.mAxis;
            const osg::Vec3f swung = axis ^ direction;
            const float swing = swung.length();
            if (swing <= 1.0e-4f)
                return false;

            const osg::Vec3f across = swung * (emitter.mWidth * sprite.mRadius / swing);
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

        /// A billboard emitter and an oriented one, and the sprites they hold.
        struct Layer
        {
            std::vector<Shaders::GpuSprite> mSprites;
            std::vector<Shaders::GpuEmitter> mEmitters;

            /// @param width nought for a billboard, and then `axis` is nought too.
            void addEmitter(float width, const osg::Vec3f& axis)
            {
                Shaders::GpuEmitter emitter{};
                emitter.mFirst = static_cast<std::uint32_t>(mSprites.size());
                emitter.mCount = 0;
                emitter.mWidth = width;
                mEmitters.push_back(emitter);

                mAxis = axis;
            }

            void addSprite(const osg::Vec3f& position, float radius)
            {
                Shaders::GpuSprite sprite{};
                sprite.mPosition = position;
                sprite.mRadius = radius;
                sprite.mAxis = mAxis;
                sprite.mEmitter = static_cast<std::uint32_t>(mEmitters.size() - 1);
                mSprites.push_back(sprite);
                ++mEmitters.back().mCount;
            }

            /// What the emitter last added hangs its quads on, which every sprite of it carries.
            osg::Vec3f mAxis;
        };

        /// The list the pass made, read back whole, beside what it reported and the rectangles it
        /// made it from.
        struct Binned
        {
            std::vector<std::uint32_t> mList;
            std::vector<std::uint64_t> mRects;
            std::uint32_t mAcross = 0;
            std::uint32_t mDown = 0;
            std::uint32_t mReport = 0;

            std::size_t getTileCount() const { return std::size_t{ mAcross } * mDown; }

            bool isUnbinned() const { return mList[0] == Shaders::SPRITE_LIST_UNBINNED; }

            std::span<const std::uint32_t> getRun(std::size_t tile) const
            {
                return std::span<const std::uint32_t>(mList).subspan(mList[tile], mList[tile + 1] - mList[tile]);
            }

            /// How many entries the runs hold between them.
            std::size_t getEntryCount() const { return mList[getTileCount()] - mList[0]; }

            /// Whether `sprite` is among what was binned into the tile `(x, y)` falls in.
            bool binnedFor(std::uint32_t sprite, std::uint32_t x, std::uint32_t y) const
            {
                const std::span<const std::uint32_t> run
                    = getRun(std::size_t{ y / Shaders::SPRITE_TILE } * mAcross + x / Shaders::SPRITE_TILE);

                return std::find(run.begin(), run.end(), sprite) != run.end();
            }

            /// Whether `sprite`'s rectangle, as the pass wrote it, holds `tile`.
            bool rectHolds(std::uint32_t sprite, std::size_t tile) const
            {
                const std::uint32_t from = static_cast<std::uint32_t>(mRects[sprite]);
                const std::uint32_t to = static_cast<std::uint32_t>(mRects[sprite] >> 32);
                const std::uint32_t x = static_cast<std::uint32_t>(tile % mAcross);
                const std::uint32_t y = static_cast<std::uint32_t>(tile / mAcross);

                return x >= (from & 0xFFFFu) && x <= (to & 0xFFFFu) && y >= (from >> 16) && y <= (to >> 16);
            }

            /// **Every run is exactly the sprites whose rectangle holds its tile, ascending.** The
            /// pass over sprites counts by the rectangle and the pass over tiles fills by it, so a
            /// run that dropped, doubled or misplaced an entry is a disagreement between the two —
            /// and the march would read it as a sprite that stopped being drawn, or one drawn out
            /// of order. Checked for every bin here, because the fill's lanes agree on their order
            /// through shared memory and a race there is exactly what this would show.
            void expectRunsMatchRects(std::uint32_t sprites) const
            {
                for (std::size_t tile = 0; tile < getTileCount(); ++tile)
                {
                    std::vector<std::uint32_t> expected;
                    for (std::uint32_t sprite = 0; sprite < sprites; ++sprite)
                        if (rectHolds(sprite, tile))
                            expected.push_back(sprite);

                    const std::span<const std::uint32_t> run = getRun(tile);
                    EXPECT_TRUE(std::equal(run.begin(), run.end(), expected.begin(), expected.end()))
                        << "tile " << tile << " holds " << run.size() << " entries where its rectangles say "
                        << expected.size();
                }
            }
        };

        template <class T>
        Buffer upload(const Device& device, std::span<const T> rows)
        {
            Buffer held = Buffer::hostWritten(
                device, std::max<VkDeviceSize>(rows.size_bytes(), 1), VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
            held.write(rows);

            return held;
        }

        struct RtxSpriteBinPassTest : Testing::DeviceTest
        {
            /// Bins `layer` under `constants` into a list with room for `capacity` entries after
            /// its starts, and reads the whole of it back.
            Binned bin(const Layer& layer, const Shaders::VisibilityConstants& constants, std::uint32_t capacity)
            {
                Device& device = getDevice();
                const SpriteBinPass pass(device, Testing::getShaderDirectory());

                Binned result;
                result.mAcross = Shaders::spriteTilesOver(constants.mCamera.mWidth);
                result.mDown = Shaders::spriteTilesOver(constants.mCamera.mHeight);

                const auto count = static_cast<std::uint32_t>(layer.mSprites.size());
                const std::size_t words = result.getTileCount() + 1 + capacity;

                const Buffer sprites = upload(device, std::span<const Shaders::GpuSprite>(layer.mSprites));
                const Buffer emitters = upload(device, std::span<const Shaders::GpuEmitter>(layer.mEmitters));
                // Staging, because the test reads them back: the renderer's own list and rectangles
                // are never read by the host and live in memory the host cannot read.
                const Buffer rects = Buffer::staging(device, std::max<VkDeviceSize>(count, 1) * sizeof(std::uint64_t),
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
                const Buffer list = Buffer::staging(device, words * sizeof(std::uint32_t),
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
                const Buffer report
                    = Buffer::staging(device, sizeof(std::uint32_t), VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);

                getPool().submitAndWait([&](VkCommandBuffer commands) {
                    pass.record(commands,
                        Shaders::SpriteBinConstants{
                            .mSprites = sprites.getDeviceAddress(),
                            .mEmitters = emitters.getDeviceAddress(),
                            .mRects = rects.getDeviceAddress(),
                            .mList = list.getDeviceAddress(),
                            .mReport = report.getDeviceAddress(),
                            .mOrigin = constants.mOrigin,
                            .mCamera = constants.mCamera,
                            .mCount = count,
                            .mCapacity = capacity,
                        },
                        list, nullptr);
                });

                result.mList.resize(words);
                std::memcpy(result.mList.data(), list.map(), words * sizeof(std::uint32_t));
                std::memcpy(&result.mReport, report.map(), sizeof(result.mReport));
                result.mRects.resize(count);
                std::memcpy(result.mRects.data(), rects.map(), count * sizeof(std::uint64_t));

                if (!result.isUnbinned())
                    result.expectRunsMatchRects(count);

                return result;
            }
        };

        /// **The whole property, checked against the march itself.** A tile's list has to hold every
        /// sprite any ray through it can meet, because the shader's own test is a refinement and
        /// never a correction — a sprite the binning drops is a raindrop that stops being drawn.
        ///
        /// Every pixel of a small frame, against every sprite, both kinds of quad and a jittered
        /// camera.
        TEST_F(RtxSpriteBinPassTest, aTileHoldsEverySpriteAnyRayThroughItCanMeet)
        {
            Layer layer;

            // Billboards spread across the view and in depth, including one that grazes the edge.
            layer.addEmitter(0.0f, osg::Vec3f());
            for (float x : { 20.0f, 60.0f, 200.0f })
                for (float y : { -30.0f, 0.0f, 17.0f })
                    for (float z : { -12.0f, 0.0f, 9.0f })
                        layer.addSprite(osg::Vec3f(x, y, z), 4.0f);

            // A rain streak: a tenth as wide as it is long, falling straight down.
            layer.addEmitter(0.1f, osg::Vec3f(0.0f, 0.0f, -1.0f));
            for (float x : { 30.0f, 90.0f })
                for (float y : { -20.0f, 5.0f, 25.0f })
                    layer.addSprite(osg::Vec3f(x, y, 3.0f), 6.0f);

            // Streaks long enough that both their ends leave the frame while their middles cross
            // it, which is the case a bound clipped end by end loses: neither cap is on the screen
            // and the cylinder between them runs down the centre of it.
            for (float x : { 6.0f, 25.0f })
                layer.addSprite(osg::Vec3f(x, 0.0f, 0.0f), 20.0f);

            // And one leaning through the plane the eye stands in, where there is no projected
            // segment to bound at all.
            layer.addEmitter(0.1f, osg::Vec3f(1.0f, 0.0f, -1.0f));
            layer.addSprite(osg::Vec3f(2.0f, 0.0f, 0.0f), 6.0f);

            for (const osg::Vec2f jitter : { osg::Vec2f(0.0f, 0.0f), osg::Vec2f(0.49f, -0.49f) })
            {
                Shaders::VisibilityConstants constants = lookingAlongX();
                constants.mCamera.mJitter = jitter;

                const Binned tiles = bin(layer, constants, sPlenty);
                ASSERT_FALSE(tiles.isUnbinned());
                ASSERT_EQ(tiles.mAcross, (sWidth + Shaders::SPRITE_TILE - 1) / Shaders::SPRITE_TILE);
                ASSERT_EQ(tiles.mDown, (sHeight + Shaders::SPRITE_TILE - 1) / Shaders::SPRITE_TILE);
                EXPECT_EQ(tiles.mReport, tiles.getEntryCount());

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
                            EXPECT_TRUE(tiles.binnedFor(at, x, y))
                                << "sprite " << at << " met at pixel " << x << ", " << y;
                        }
                    }

                // A property nothing meets is a property nothing checks.
                EXPECT_GT(met, 200u) << "the fixture stopped covering the frame";
            }
        }

        /// A tile's run ascends, because that is the order the march composites in.
        ///
        /// Sprites blend in the order they are walked, and the loop the tiles replaced walked
        /// emitters in order and indices within one. Sprite indices are contiguous per emitter, so
        /// ascending index is that same order — which is what lets a byte comparison check the change
        /// at all. **Every entry and not only within a run**: a run out of order and a run holding
        /// what its neighbour counted are both a list the march reads wrongly, so what the runs hold
        /// between them is checked against the sprites too.
        TEST_F(RtxSpriteBinPassTest, aTilesRunAscendsSoTheCompositeOrderIsTheOneTheMarchKept)
        {
            Layer layer;
            layer.addEmitter(0.0f, osg::Vec3f());
            for (float y : { -8.0f, -4.0f, 0.0f, 4.0f, 8.0f })
                layer.addSprite(osg::Vec3f(40.0f, y, 0.0f), 30.0f);

            const Binned tiles = bin(layer, lookingAlongX(), sPlenty);
            ASSERT_FALSE(tiles.isUnbinned());

            std::uint32_t runs = 0;
            for (std::size_t tile = 0; tile < tiles.getTileCount(); ++tile)
            {
                const std::span<const std::uint32_t> run = tiles.getRun(tile);
                for (const std::uint32_t sprite : run)
                    EXPECT_LT(sprite, layer.mSprites.size()) << "tile " << tile << " names a sprite that is not";

                if (run.size() < 2)
                    continue;

                ++runs;
                for (std::size_t at = 1; at < run.size(); ++at)
                    EXPECT_LT(run[at - 1], run[at]) << "tile " << tile;
            }

            EXPECT_GT(runs, 0u) << "no tile held more than one sprite, so nothing was ordered";
        }

        /// A sprite the eye is inside covers whatever it likes, and there are no tangent lines to work
        /// that out from — so it goes in every tile rather than being reasoned about.
        TEST_F(RtxSpriteBinPassTest, aSpriteAroundTheEyeIsInEveryTile)
        {
            Layer layer;
            layer.addEmitter(0.0f, osg::Vec3f());
            layer.addSprite(osg::Vec3f(1.0f, 0.0f, 0.0f), 50.0f);

            const Binned tiles = bin(layer, lookingAlongX(), sPlenty);
            ASSERT_FALSE(tiles.isUnbinned());

            ASSERT_EQ(tiles.getEntryCount(), tiles.getTileCount());
            for (std::size_t tile = 0; tile < tiles.getTileCount(); ++tile)
                EXPECT_EQ(tiles.getRun(tile).size(), 1u) << "tile " << tile;
        }

        /// A rain streak falling past the camera reaches the strip it covers and not the whole frame.
        ///
        /// **The counterpart to the test above, and the reason a capsule bounds a streak.** A ball
        /// around the quad's corners is as wide as the streak is long, so a drop this close was a ball
        /// the eye stood inside and went into every tile on the screen — ninety-four drops out of a
        /// storm's two thousand six hundred, and 96% of the whole index table. The capsule's radius is
        /// the streak's width, which is a tenth of that here and a fiftieth in the content.
        TEST_F(RtxSpriteBinPassTest, aStreakFallingPastTheEyeReachesTheStripItCoversAndNoMore)
        {
            Layer layer;
            layer.addEmitter(0.1f, osg::Vec3f(0.0f, 0.0f, -1.0f));
            layer.addSprite(osg::Vec3f(6.0f, 0.0f, 0.0f), 8.0f);

            const Binned tiles = bin(layer, lookingAlongX(), sPlenty);
            ASSERT_FALSE(tiles.isUnbinned());

            // Eight tenths of a unit of width at six away is a tangent of 0.133, against the 0.77 the
            // frame's own half-width is — so the streak is a sixth of the frame across, dead centre,
            // and its own length takes it off the top and the bottom.
            EXPECT_TRUE(tiles.binnedFor(0, sWidth / 2, sHeight / 2));
            for (std::uint32_t y = 0; y < sHeight; ++y)
            {
                EXPECT_FALSE(tiles.binnedFor(0, 0, y)) << "the left edge held a streak down the middle";
                EXPECT_FALSE(tiles.binnedFor(0, sWidth - 1, y)) << "the right edge held a streak down the middle";
            }

            // Every tile is what the ball around the corners gave, and it is what this is measured
            // against: the strip is two of the four tile columns and all three rows.
            EXPECT_EQ(tiles.getEntryCount(), tiles.getTileCount() / 2);
        }

        /// A sprite behind the eye reaches no tile, and one off to the side reaches only its own.
        TEST_F(RtxSpriteBinPassTest, whatTheFrameCannotSeeIsBinnedNowhereAndTheRestIsBinnedNarrowly)
        {
            Layer layer;
            layer.addEmitter(0.0f, osg::Vec3f());
            layer.addSprite(osg::Vec3f(-200.0f, 0.0f, 0.0f), 4.0f);
            layer.addSprite(osg::Vec3f(100.0f, 0.0f, 0.0f), 2.0f);

            const Binned tiles = bin(layer, lookingAlongX(), sPlenty);
            ASSERT_FALSE(tiles.isUnbinned());

            for (std::size_t tile = 0; tile < tiles.getTileCount(); ++tile)
                for (const std::uint32_t index : tiles.getRun(tile))
                    EXPECT_EQ(index, 1u) << "the sprite behind the eye reached tile " << tile;

            // Two units across at a hundred away is under a pixel of a sixty-degree frame, so the
            // slack the jitter needs is the whole of what it covers: four tiles at the very most.
            EXPECT_GT(tiles.getEntryCount(), 0u);
            EXPECT_LE(tiles.getEntryCount(), 4u);
        }

        /// The orthographic camera slides the eye instead of turning the ray, so a sprite's tiles are
        /// where it stands rather than where it points.
        TEST_F(RtxSpriteBinPassTest, theOrthographicCameraBinsWhereTheSpriteStands)
        {
            Layer layer;
            layer.addEmitter(0.0f, osg::Vec3f());
            layer.addSprite(osg::Vec3f(50.0f, 0.0f, 0.0f), 2.0f);

            osg::Matrixf view;
            view.makeLookAt(osg::Vec3f(0.0f, 0.0f, 0.0f), osg::Vec3f(1.0f, 0.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 1.0f));
            const Shaders::VisibilityConstants constants
                = makeOrthographicCameraFromView(view, 128.0f, 96.0f, sWidth, sHeight, 1.0f, 10000.0f);

            const Binned tiles = bin(layer, constants, sPlenty);
            ASSERT_FALSE(tiles.isUnbinned());

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

                    EXPECT_TRUE(tiles.binnedFor(0, x, y)) << "pixel " << x << ", " << y;
                }

            EXPECT_GT(tiles.getEntryCount(), 0u);
        }

        /// Hundreds of sprites over a frame of hundreds of tiles, so the fill walks many strides of
        /// them and cycles its shared words round and round.
        ///
        /// **The fixtures above are two strides at most, and the cycle is three words long**, so
        /// none of them ever reused a word — which is the one thing that can go wrong in a fill that
        /// clears a word while the lanes are between barriers. A storm's worth of small billboards
        /// and streaks, a few puffs around the eye that reach every tile, and the property checked
        /// on top of the runs matching their rectangles.
        TEST_F(RtxSpriteBinPassTest, aStormOfSpritesFillsEveryRunAcrossManyStrides)
        {
            constexpr std::uint32_t width = 320;
            constexpr std::uint32_t height = 240;

            Layer layer;

            layer.addEmitter(0.0f, osg::Vec3f());
            for (float x : { 30.0f, 60.0f, 120.0f, 250.0f })
                for (int y = -12; y <= 12; ++y)
                    for (int z = -8; z <= 8; z += 2)
                        layer.addSprite(
                            osg::Vec3f(x, static_cast<float>(y) * 4.0f, static_cast<float>(z) * 3.0f), 1.5f);

            layer.addEmitter(0.1f, osg::Vec3f(0.0f, 0.0f, -1.0f));
            for (float x : { 25.0f, 75.0f, 150.0f })
                for (int y = -10; y <= 10; ++y)
                    layer.addSprite(osg::Vec3f(x, static_cast<float>(y) * 5.0f, 2.0f), 4.0f);

            layer.addEmitter(0.0f, osg::Vec3f());
            for (float x : { 1.0f, 3.0f, 5.0f })
                layer.addSprite(osg::Vec3f(x, 0.0f, 0.0f), 40.0f);

            ASSERT_GT(layer.mSprites.size(), 6 * Shaders::SPRITE_RUNS_LANES)
                << "the fixture stopped being many strides";

            Shaders::VisibilityConstants constants = makeCamera(
                osg::Vec3f(0.0f, 0.0f, 0.0f), osg::Vec3f(1.0f, 0.0f, 0.0f), 60.0f, width, height, 10000.0f);
            constants.mCamera.mJitter = osg::Vec2f(-0.3f, 0.45f);

            const Binned tiles = bin(layer, constants, 1u << 20);
            ASSERT_FALSE(tiles.isUnbinned());
            EXPECT_EQ(tiles.mReport, tiles.getEntryCount());

            // The three puffs around the eye are in every run, and every run ascends.
            for (std::size_t tile = 0; tile < tiles.getTileCount(); ++tile)
            {
                const std::span<const std::uint32_t> run = tiles.getRun(tile);
                ASSERT_GE(run.size(), 3u) << "tile " << tile;
                for (std::size_t at = 1; at < run.size(); ++at)
                    EXPECT_LT(run[at - 1], run[at]) << "tile " << tile;
                for (std::size_t at = 0; at < 3; ++at)
                    EXPECT_EQ(run[run.size() - 3 + at], layer.mSprites.size() - 3 + at) << "tile " << tile;
            }

            std::uint32_t met = 0;
            for (std::uint32_t y = 0; y < height; y += 3)
                for (std::uint32_t x = 0; x < width; x += 3)
                {
                    const osg::Vec3f direction = rayThrough(constants.mCamera, x, y);

                    for (std::uint32_t at = 0; at < layer.mSprites.size(); ++at)
                    {
                        if (!marchWouldMeet(layer.mSprites[at], layer.mEmitters[layer.mSprites[at].mEmitter],
                                constants.mOrigin, direction))
                            continue;

                        ++met;
                        EXPECT_TRUE(tiles.binnedFor(at, x, y)) << "sprite " << at << " met at pixel " << x << ", " << y;
                    }
                }

            EXPECT_GT(met, 1000u) << "the fixture stopped covering the frame";
        }

        /// The frame the game traces, over an interior's worth of candle smoke: nineteen columns of
        /// puffs, three of them rising past the eye, every run matching its rectangles.
        ///
        /// **The frame's own tile count, because a fill over three hundred tiles and one over eight
        /// thousand are different dispatches.** A thousand workgroups, the last of them partly past
        /// the last tile, and runs eight thousand entries long where the puffs about the eye reach
        /// every tile.
        TEST_F(RtxSpriteBinPassTest, aRoomOfCandleSmokeFillsEveryRunAtTheGamesFrameSize)
        {
            constexpr std::uint32_t width = 1920;
            constexpr std::uint32_t height = 1080;

            Layer layer;
            for (int column = 0; column < 19; ++column)
            {
                layer.addEmitter(0.0f, osg::Vec3f());

                const float x
                    = column < 3 ? 2.0f + static_cast<float>(column) : 40.0f + static_cast<float>(column) * 9.0f;
                const float y = column < 3 ? 0.0f : static_cast<float>(column % 5) * 20.0f - 40.0f;
                for (int puff = 0; puff < 20; ++puff)
                    layer.addSprite(
                        osg::Vec3f(x, y, static_cast<float>(puff) * 6.0f - 30.0f), column < 3 ? 25.0f : 8.0f);
            }

            const Shaders::VisibilityConstants constants = makeCamera(
                osg::Vec3f(0.0f, 0.0f, 0.0f), osg::Vec3f(1.0f, 0.0f, 0.0f), 60.0f, width, height, 10000.0f);

            // Sixty puffs in every tile is most of the list, at any tile size the frame may have.
            const Binned tiles = bin(layer, constants, 1u << 22);
            ASSERT_FALSE(tiles.isUnbinned());
            EXPECT_EQ(tiles.mReport, tiles.getEntryCount());

            std::size_t longest = 0;
            for (std::size_t tile = 0; tile < tiles.getTileCount(); ++tile)
                longest = std::max(longest, tiles.getRun(tile).size());

            // The sixty puffs about the eye, at the least, in every tile.
            EXPECT_GE(longest, 60u);
        }

        /// A list with no room for its runs says so in its first entry and names the count in its
        /// second, reports what it needed, and is whole again once given that much.
        ///
        /// **The report is what sizes the next frame's list, so it has to be the same number whether
        /// or not the runs fit** — and the list given exactly that room has to be the list a
        /// generous one would have made, entry for entry.
        TEST_F(RtxSpriteBinPassTest, aListTooSmallForItsRunsSaysSoAndNamesWhatItNeeded)
        {
            Layer layer;
            layer.addEmitter(0.0f, osg::Vec3f());
            for (float y : { -8.0f, -4.0f, 0.0f, 4.0f, 8.0f })
                layer.addSprite(osg::Vec3f(40.0f, y, 0.0f), 30.0f);

            const Shaders::VisibilityConstants constants = lookingAlongX();

            const Binned generous = bin(layer, constants, sPlenty);
            ASSERT_FALSE(generous.isUnbinned());
            const std::size_t needed = generous.getEntryCount();
            ASSERT_GT(needed, 1u);
            EXPECT_EQ(generous.mReport, needed);

            const Binned starved = bin(layer, constants, static_cast<std::uint32_t>(needed - 1));
            EXPECT_TRUE(starved.isUnbinned());
            EXPECT_EQ(starved.mList[1], layer.mSprites.size());
            EXPECT_EQ(starved.mReport, needed);

            const Binned exact = bin(layer, constants, static_cast<std::uint32_t>(needed));
            ASSERT_FALSE(exact.isUnbinned());
            EXPECT_EQ(exact.mReport, needed);

            const std::size_t whole = exact.getTileCount() + 1 + needed;
            ASSERT_EQ(exact.mList.size(), whole);
            EXPECT_TRUE(std::equal(exact.mList.begin(), exact.mList.end(), generous.mList.begin()))
                << "the list given exactly its room differs from the one given plenty";
        }

        /// No sprites is every start at the head's end and nothing reported, so a frame with none
        /// reads no run at all and the next one asks for no room.
        TEST_F(RtxSpriteBinPassTest, noSpritesIsEveryStartAtTheHeadsEnd)
        {
            const Layer layer;
            const Binned tiles = bin(layer, lookingAlongX(), 0);

            ASSERT_FALSE(tiles.isUnbinned());
            for (std::size_t tile = 0; tile <= tiles.getTileCount(); ++tile)
                EXPECT_EQ(tiles.mList[tile], tiles.getTileCount() + 1) << "start " << tile;

            EXPECT_EQ(tiles.mReport, 0u);
        }
    }

    namespace
    {
        /// The pass, the pool it is recorded into, and what it makes of a world.
        ///
        /// **One pass for the whole fixture**, because building it compiles a pipeline and every test
        /// below hands it a different world rather than a different pass.
        struct Shading
        {
            const Device& mDevice;
            CommandPool mPool;
            SpriteShadePass mPass;

            explicit Shading(const Device& device)
                : mDevice(device)
                , mPool(device)
                , mPass(device, Testing::getShaderDirectory())
            {
            }

            /// Runs the shading over `sprites` and `emitters` and gives the sprites back shaded.
            ///
            /// The tables are staged rather than host-written, because this reads them back:
            /// `Buffer::hostWritten` is write-combining memory and `map` refuses a read of it.
            std::vector<Shaders::GpuSprite> over(std::span<const Shaders::GpuSprite> sprites,
                std::span<const Shaders::GpuEmitter> emitters, const osg::Vec3f& toSun)
            {
                constexpr VkBufferUsageFlags usage
                    = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

                const Buffer spriteTable = Buffer::staging(mDevice, sprites.size_bytes(), usage);
                const Buffer emitterTable = Buffer::hostWritten(mDevice, emitters.size_bytes(), usage);
                const Buffer order = Buffer::deviceLocal(
                    mDevice, sprites.size() * Shaders::SPRITE_SHADE_LIGHTS * sizeof(std::uint64_t), usage);

                spriteTable.writeAt(0, sprites);
                emitterTable.writeAt(0, emitters);

                const Shaders::SpriteShadeConstants shade{
                    .mSprites = spriteTable.getDeviceAddress(),
                    .mEmitters = emitterTable.getDeviceAddress(),
                    .mOrder = order.getDeviceAddress(),
                    .mToSun = toSun,
                    .mEmitterCount = static_cast<std::uint32_t>(emitters.size()),
                    .mCount = static_cast<std::uint32_t>(sprites.size()),
                };

                mPool.submitAndWait([&](VkCommandBuffer commands) { mPass.record(commands, shade, nullptr); });

                std::vector<Shaders::GpuSprite> back(sprites.size());
                std::memcpy(back.data(), spriteTable.map(), sprites.size_bytes());

                return back;
            }
        };

        /// One emitter and its run, built so the grid is one unit a cell.
        ///
        /// **A reach of sixteen over thirty-two cells is a cell of one unit**, as long as no sprite is
        /// wider than eight — which is what makes every figure below a plain number. The centre sits at
        /// the origin, so a sprite's world position is its offset on the grid.
        struct Column
        {
            std::vector<Shaders::GpuSprite> mSprites;
            Shaders::GpuEmitter mEmitter{};

            Column()
            {
                mEmitter.mCentre = osg::Vec3f();
                mEmitter.mReach = 16.0f;
            }

            void add(const osg::Vec3f& position, float radius, float alpha)
            {
                Shaders::GpuSprite sprite{};
                sprite.mPosition = position;
                sprite.mRadius = radius;
                sprite.mAlpha = alpha;
                mSprites.push_back(sprite);
                mEmitter.mCount = static_cast<std::uint32_t>(mSprites.size());
            }

            void shade(Shading& shading, const osg::Vec3f& toSun)
            {
                mSprites = shading.over(mSprites, std::span<const Shaders::GpuEmitter>(&mEmitter, 1), toSun);
            }
        };

        /// Several columns laid end to end as one table, the way `Rtx::SceneDesc` lays one out:
        /// every run in one array, and each emitter naming where its own begins.
        struct Table
        {
            std::vector<Shaders::GpuSprite> mSprites;
            std::vector<Shaders::GpuEmitter> mEmitters;

            void add(Column column)
            {
                column.mEmitter.mFirst = static_cast<std::uint32_t>(mSprites.size());
                mEmitters.push_back(column.mEmitter);
                mSprites.insert(mSprites.end(), column.mSprites.begin(), column.mSprites.end());
            }

            void shade(Shading& shading, const osg::Vec3f& toSun)
            {
                mSprites = shading.over(mSprites, mEmitters, toSun);
            }
        };

        const osg::Vec3f sEast(1.0f, 0.0f, 0.0f);

        /// Everything below drives the one implementation there is, on the device it runs on.
        ///
        /// **There is no host reference to check against, and that is deliberate.** A second
        /// implementation of one computation is two things to keep in step; what stands in its place
        /// is that every figure below is worked out by hand from what `spriteshade.h` says a layer
        /// count is.
        struct RtxSpriteShadeTest : Testing::DeviceTest
        {
            /// **Made on the first ask and not as a member**, which is `DeviceTest::getPool`'s own
            /// shape and for its reason: the device arrives in `SetUp`, so a member initialiser
            /// would build the pass against a harness that is not there yet.
            Shading& shading()
            {
                if (mShading == nullptr)
                    mShading = std::make_unique<Shading>(getDevice());

                return *mShading;
            }

        private:
            std::unique_ptr<Shading> mShading;
        };

        /// A sprite behind another along the light reads that one's fade, and the one in front reads
        /// nothing.
        ///
        /// The near disc is eight in radius, and the far sprite's point sits a unit off its axis between
        /// two cells — both well inside the disc, where the footprint is whole, so the read is exactly
        /// the fade and not a rim's fraction of it. The near sprite is that unit lower, so from the sky
        /// it is the far one, and the other's disc of four does not reach it six across.
        TEST_F(RtxSpriteShadeTest, aSpriteBehindAnotherReadsItsFade)
        {
            Column column;
            column.add(osg::Vec3f(6.0f, 0.0f, -1.0f), 8.0f, 0.75f);
            column.add(osg::Vec3f(0.0f, 0.0f, 0.0f), 4.0f, 1.0f);
            column.shade(shading(), sEast);

            EXPECT_FLOAT_EQ(column.mSprites[0].mSunLayers, 0.0f) << "nothing is nearer the sun";
            EXPECT_FLOAT_EQ(column.mSprites[1].mSunLayers, 0.75f) << "the near sprite's fade, once";
            EXPECT_FLOAT_EQ(column.mSprites[0].mSkyLayers, 0.0f) << "the higher one's disc does not reach it";
            EXPECT_FLOAT_EQ(column.mSprites[1].mSkyLayers, 0.0f) << "nothing is higher";
        }

        /// Layers add along the light, and the order the sprites arrive in is not the order they shade in.
        ///
        /// Three on the axis at twelve, six and nought, fading a half, a quarter and one: the last reads
        /// three quarters, the middle a half, the first nothing — from either end of the array.
        TEST_F(RtxSpriteShadeTest, layersAddUpAlongTheLightWhateverTheOrder)
        {
            const auto build = [this](bool reversed) {
                Column column;
                const std::array<float, 3> along{ 12.0f, 6.0f, 0.0f };
                const std::array<float, 3> fade{ 0.5f, 0.25f, 1.0f };
                for (std::size_t i = 0; i < 3; ++i)
                {
                    const std::size_t at = reversed ? 2 - i : i;
                    column.add(osg::Vec3f(along[at], 0.0f, 0.0f), 4.0f, fade[at]);
                }
                column.shade(shading(), sEast);
                return column;
            };

            const Column forward = build(false);
            EXPECT_FLOAT_EQ(forward.mSprites[0].mSunLayers, 0.0f);
            EXPECT_FLOAT_EQ(forward.mSprites[1].mSunLayers, 0.5f);
            EXPECT_FLOAT_EQ(forward.mSprites[2].mSunLayers, 0.75f);

            const Column backward = build(true);
            EXPECT_FLOAT_EQ(backward.mSprites[2].mSunLayers, 0.0f);
            EXPECT_FLOAT_EQ(backward.mSprites[1].mSunLayers, 0.5f);
            EXPECT_FLOAT_EQ(backward.mSprites[0].mSunLayers, 0.75f);
        }

        /// A sprite beside the light's path to another is not in it.
        ///
        /// The near disc is two in radius and the far sprite four to the side of the axis: the rim's
        /// one-cell ramp reaches `2 + 0.5` and stops short of it.
        TEST_F(RtxSpriteShadeTest, aSpriteBesideTheLightsPathIsNotInIt)
        {
            Column column;
            column.add(osg::Vec3f(6.0f, 0.0f, 0.0f), 2.0f, 1.0f);
            column.add(osg::Vec3f(0.0f, 4.0f, 0.0f), 2.0f, 1.0f);
            column.shade(shading(), sEast);

            EXPECT_FLOAT_EQ(column.mSprites[1].mSunLayers, 0.0f);
        }

        /// A disc too small to reach a cell's centre counts its own area on the cell it is in.
        ///
        /// A radius of a quarter cell is an area of `pi / 16 = 0.19635`. Both sprites sit on a cell's
        /// centre — half a unit off the axis, since the grid's cells are centred on whole numbers from
        /// the reach's edge — so the point lands where the far sprite reads, whole.
        TEST_F(RtxSpriteShadeTest, aTinyDiscCountsItsArea)
        {
            Column column;
            column.add(osg::Vec3f(6.0f, -0.5f, -0.5f), 0.25f, 1.0f);
            column.add(osg::Vec3f(0.0f, -0.5f, -0.5f), 8.0f, 1.0f);
            column.shade(shading(), sEast);

            EXPECT_NEAR(column.mSprites[1].mSunLayers, 0.19635f, 1.0e-4f);
            EXPECT_FLOAT_EQ(column.mSprites[0].mSunLayers, 0.0f);
        }

        /// The sky is straight up, whatever the sun does.
        ///
        /// One sprite six units over another and one unit further from a low sun: the lower reads the
        /// upper's fade from the sky and nothing from the sun, and the upper reads nothing from either —
        /// the lower's disc is four in radius and the upper's path to the sun passes six above it.
        TEST_F(RtxSpriteShadeTest, theSkyIsStraightUp)
        {
            Column column;
            column.add(osg::Vec3f(-1.0f, 0.0f, 6.0f), 8.0f, 0.5f);
            column.add(osg::Vec3f(0.0f, 0.0f, 0.0f), 4.0f, 1.0f);
            column.shade(shading(), sEast);

            EXPECT_FLOAT_EQ(column.mSprites[1].mSkyLayers, 0.5f);
            EXPECT_FLOAT_EQ(column.mSprites[1].mSunLayers, 0.0f);
            EXPECT_FLOAT_EQ(column.mSprites[0].mSkyLayers, 0.0f);
            EXPECT_FLOAT_EQ(column.mSprites[0].mSunLayers, 0.0f);
        }

        /// A flame, a rain streak and a lone puff are shaded by nothing.
        ///
        /// The workgroups they get read their emitter and return, so what they leave is the nought
        /// `toGpu` built the sprites with.
        TEST_F(RtxSpriteShadeTest, flamesStreaksAndLonePuffsAreNotShaded)
        {
            Column flame;
            flame.add(osg::Vec3f(6.0f, 0.0f, 0.0f), 8.0f, 1.0f);
            flame.add(osg::Vec3f(0.0f, 0.0f, 0.0f), 4.0f, 1.0f);
            flame.mEmitter.mAdditive = 1u;
            flame.shade(shading(), sEast);
            EXPECT_FLOAT_EQ(flame.mSprites[1].mSunLayers, 0.0f) << "a flame emits and shadows nothing";

            Column rain;
            rain.add(osg::Vec3f(6.0f, 0.0f, 0.0f), 8.0f, 1.0f);
            rain.add(osg::Vec3f(0.0f, 0.0f, 0.0f), 4.0f, 1.0f);
            rain.mEmitter.mWidth = 0.1f;
            rain.shade(shading(), sEast);
            EXPECT_FLOAT_EQ(rain.mSprites[1].mSunLayers, 0.0f) << "a streak is a thin thing";

            Column lone;
            lone.add(osg::Vec3f(0.0f, 0.0f, 0.0f), 4.0f, 1.0f);
            lone.shade(shading(), sEast);
            EXPECT_FLOAT_EQ(lone.mSprites[0].mSunLayers, 0.0f);
            EXPECT_FLOAT_EQ(lone.mSprites[0].mSkyLayers, 0.0f);
        }

        /// Every cell of a disc holds the coverage the analysis gives it.
        ///
        /// **The cross-check the coverage rule needs.** Every test above reads a point deep inside a
        /// disc, where the whole footprint and its rim give the same answer. This reads the rim, which
        /// is where `clamp(radius - distance + 0.5, 0, 1)` stops being one and starts being a fraction.
        ///
        /// **A probe of no alpha reads and lays nothing**, so one disc's footprint is what the whole
        /// grid holds however many points are put into it. The disc is further along the light than any
        /// of them, so it is laid before all of them.
        ///
        /// Half a unit off each axis is a cell's centre, where the read is that cell and not a blend of
        /// four — `aTinyDiscCountsItsArea` says why. A disc of six at a half of alpha is then
        /// `0.5 * clamp(6.5 - distance, 0, 1)` at a point `distance` cells away.
        TEST_F(RtxSpriteShadeTest, everyCellOfADiscHoldsTheCoverageTheAnalysisGivesIt)
        {
            struct Probe
            {
                float mAcross;
                float mUpward;
                float mExpected;
            };

            // Along one axis, so `distance` is the offset itself: whole out to five, the rim at six, and
            // nothing at seven.
            //
            // Then the diagonal, where three across and three up is `sqrt(18) = 4.2426` — still whole —
            // and four and four is `sqrt(32) = 5.6569`, which is `0.5 * 0.8431` on the rim.
            constexpr std::array<Probe, 6> sProbes{ {
                { 0.0f, 0.0f, 0.5f },
                { 5.0f, 0.0f, 0.5f },
                { 6.0f, 0.0f, 0.25f },
                { 7.0f, 0.0f, 0.0f },
                { 3.0f, 3.0f, 0.5f },
                { 4.0f, 4.0f, 0.5f * 0.84314575f },
            } };

            Column column;
            column.add(osg::Vec3f(6.0f, -0.5f, -0.5f), 6.0f, 0.5f);
            for (const Probe& probe : sProbes)
                column.add(osg::Vec3f(0.0f, -0.5f - probe.mAcross, -0.5f - probe.mUpward), 0.0f, 0.0f);

            column.shade(shading(), sEast);

            for (std::size_t at = 0; at < sProbes.size(); ++at)
                EXPECT_NEAR(column.mSprites[at + 1].mSunLayers, sProbes[at].mExpected, 1.0e-5f)
                    << "probe " << sProbes[at].mAcross << ", " << sProbes[at].mUpward;
        }

        /// A sun off every axis still finds what stands in its way.
        ///
        /// Two sprites on the diagonal, the nearer six root two along it: the same figures as on the
        /// axis, because the grid is laid across whatever the light is.
        TEST_F(RtxSpriteShadeTest, aSunOffTheAxesShadesAlongItself)
        {
            osg::Vec3f toSun(1.0f, 1.0f, 0.0f);
            toSun.normalize();

            Column column;
            column.add(toSun * 6.0f, 8.0f, 0.75f);
            column.add(osg::Vec3f(), 4.0f, 1.0f);
            column.shade(shading(), toSun);

            EXPECT_FLOAT_EQ(column.mSprites[0].mSunLayers, 0.0f);
            EXPECT_FLOAT_EQ(column.mSprites[1].mSunLayers, 0.75f);
        }

        /// Two sprites at one depth shade in index order, and only one order is allowed.
        ///
        /// **What holds the sort's answer down.** The order is made total by breaking a tie on the
        /// index, so a sort that left equal depths in whichever order it found them would give either
        /// answer and a frame could flicker between the two. Nothing above reaches a tie.
        ///
        /// Both sprites stand at one point, so both depths are equal along the sun and along the sky.
        /// The first lays half a layer where the second then reads it, and the second lays a whole one
        /// where nothing reads. Half a unit off each axis is a cell's centre, which is what makes the
        /// read the cell itself — `aTinyDiscCountsItsArea` says why.
        TEST_F(RtxSpriteShadeTest, twoSpritesAtOneDepthShadeInIndexOrder)
        {
            Column column;
            column.add(osg::Vec3f(0.5f, -0.5f, -0.5f), 6.0f, 0.5f);
            column.add(osg::Vec3f(0.5f, -0.5f, -0.5f), 6.0f, 1.0f);
            column.shade(shading(), sEast);

            EXPECT_FLOAT_EQ(column.mSprites[0].mSunLayers, 0.0f) << "the lower index lays down first";
            EXPECT_FLOAT_EQ(column.mSprites[1].mSunLayers, 0.5f) << "and the higher one reads it";
            EXPECT_FLOAT_EQ(column.mSprites[0].mSkyLayers, 0.0f);
            EXPECT_FLOAT_EQ(column.mSprites[1].mSkyLayers, 0.5f);
        }

        /// The sort is a network over any length, so a run that is not a power of two sorts too.
        ///
        /// **What replaced the cap.** A bitonic network is defined on a power of two and has to be
        /// padded to one, which needs room past the run — a cap, and something else to shade what is
        /// past it. `sortRun` is Batcher's odd-even merge with every comparator touching a wire past
        /// the run removed, which needs no padding and so has no length it cannot take.
        ///
        /// Every run from two to thirty-three, each a ladder along the light at one alpha: sprite `k`
        /// from the light has `k` whole layers over it, which only comes out right if the whole run
        /// came out in order. Half a unit off each axis is a cell's centre, and a radius of six
        /// covers the ladder's own point whole.
        TEST_F(RtxSpriteShadeTest, aRunOfAnyLengthComesOutInDepthOrder)
        {
            for (std::uint32_t count = 2; count <= 33; ++count)
            {
                Column column;
                for (std::uint32_t at = 0; at < count; ++at)
                    column.add(osg::Vec3f(static_cast<float>(at) - 0.5f, -0.5f, -0.5f), 6.0f, 1.0f);

                column.shade(shading(), sEast);

                // Index `at` stands at `at` along the light, so the run shades from the last index
                // back: the furthest along has nothing over it and the nearest the eye has them all.
                for (std::uint32_t at = 0; at < count; ++at)
                    ASSERT_FLOAT_EQ(column.mSprites[at].mSunLayers, static_cast<float>(count - 1 - at))
                        << "run of " << count << ", sprite " << at;
            }
        }

        /// Several emitters in one table are shaded apart, and each reads only its own run.
        ///
        /// **The two lights of one emitter and the runs of two emitters all write the order buffer at
        /// once**, so this is what says none of them writes another's word: light `l`'s keys sit at
        /// `l * count`, and an emitter's own run inside that.
        ///
        /// Three emitters, each a pair on its own axis a long way from the others, and each pair the
        /// same figures as `aSpriteBehindAnotherReadsItsFade`.
        TEST_F(RtxSpriteShadeTest, emittersInOneTableAreShadedApart)
        {
            constexpr std::array<float, 3> sFades{ 0.75f, 0.5f, 0.25f };

            Table table;
            for (std::size_t which = 0; which < sFades.size(); ++which)
            {
                const osg::Vec3f centre(0.0f, 1000.0f * static_cast<float>(which), 0.0f);

                Column column;
                column.mEmitter.mCentre = centre;
                column.add(centre + osg::Vec3f(6.0f, 0.0f, 0.0f), 8.0f, sFades[which]);
                column.add(centre, 4.0f, 1.0f);
                table.add(std::move(column));
            }

            table.shade(shading(), sEast);

            for (std::size_t which = 0; which < sFades.size(); ++which)
            {
                EXPECT_FLOAT_EQ(table.mSprites[which * 2].mSunLayers, 0.0f) << "emitter " << which;
                EXPECT_FLOAT_EQ(table.mSprites[which * 2 + 1].mSunLayers, sFades[which]) << "emitter " << which;
            }
        }

        /// Counted with no layers behind it, which is what `getUnvalidatedHarness` is for: the
        /// validation layer allocates eleven times per recorded command buffer, and none of those
        /// are this renderer's.
        struct RtxSpriteShadeAllocationTest : Testing::DeviceTest
        {
            RtxSpriteShadeAllocationTest()
                : Testing::DeviceTest(/*validation=*/false)
            {
            }
        };

        /// Recording the shading a second time goes to the heap not at all.
        ///
        /// **This runs on every frame.** What the pass records is a barrier, a bind, a push and a
        /// dispatch, and none of them may reach the allocator — a `std::string` for a debug label or
        /// a vector of barriers built per call would be a per-frame allocation on the frame path.
        ///
        /// Recorded twice into one command buffer, and the first is what warms whatever legitimately
        /// allocates once. Both runs are real: the second shades the same world over again, which
        /// costs the device a dispatch and changes nothing about the answer.
        TEST_F(RtxSpriteShadeAllocationTest, recordingTheShadingAgainDoesNotTouchTheHeap)
        {
            constexpr VkBufferUsageFlags usage
                = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

            Column column;
            column.add(osg::Vec3f(6.0f, 0.0f, -1.0f), 8.0f, 0.75f);
            column.add(osg::Vec3f(0.0f, 0.0f, 0.0f), 4.0f, 1.0f);

            Shading shading(getDevice());

            const std::span<const Shaders::GpuSprite> sprites(column.mSprites);
            const Buffer spriteTable = Buffer::staging(getDevice(), sprites.size_bytes(), usage);
            const Buffer emitterTable = Buffer::hostWritten(getDevice(), sizeof(Shaders::GpuEmitter), usage);
            const Buffer order = Buffer::deviceLocal(
                getDevice(), sprites.size() * Shaders::SPRITE_SHADE_LIGHTS * sizeof(std::uint64_t), usage);

            spriteTable.writeAt(0, sprites);
            emitterTable.writeAt(0, std::span<const Shaders::GpuEmitter>(&column.mEmitter, 1));

            const Shaders::SpriteShadeConstants shade{
                .mSprites = spriteTable.getDeviceAddress(),
                .mEmitters = emitterTable.getDeviceAddress(),
                .mOrder = order.getDeviceAddress(),
                .mToSun = sEast,
                .mEmitterCount = 1,
                .mCount = static_cast<std::uint32_t>(sprites.size()),
            };

            std::size_t spent = 0;
            shading.mPool.submitAndWait([&](VkCommandBuffer commands) {
                shading.mPass.record(commands, shade, nullptr);

                const std::size_t before = Testing::getAllocationCount();
                shading.mPass.record(commands, shade, nullptr);
                spent = Testing::getAllocationCount() - before;
            });

            EXPECT_EQ(spent, 0u) << spent << " allocations to record the shading";

            std::vector<Shaders::GpuSprite> back(sprites.size());
            std::memcpy(back.data(), spriteTable.map(), sprites.size_bytes());
            EXPECT_FLOAT_EQ(back[1].mSunLayers, 0.75f) << "and the same answer as a single recording";
        }
    }
}
