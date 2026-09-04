#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/camera.hpp>
#include <components/rtx/shaders/spritebin.h>
#include <components/rtxvulkan/buffer.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/spritebinpass.hpp>

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

            // Streaks long enough that both their ends leave the frame while their middles cross
            // it, which is the case a bound clipped end by end loses: neither cap is on the screen
            // and the cylinder between them runs down the centre of it.
            for (float x : { 6.0f, 25.0f })
                layer.addSprite(osg::Vec3f(x, 0.0f, 0.0f), 20.0f);

            // And one leaning through the plane the eye stands in, where there is no projected
            // segment to bound at all.
            layer.addEmitter(osg::Vec3f(0.1f, 0.0f, 0.0f), osg::Vec3f(1.0f, 0.0f, -1.0f));
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
            layer.addEmitter(osg::Vec3f(), osg::Vec3f());
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
            layer.addEmitter(osg::Vec3f(), osg::Vec3f());
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
            layer.addEmitter(osg::Vec3f(0.1f, 0.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, -1.0f));
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
            layer.addEmitter(osg::Vec3f(), osg::Vec3f());
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
            layer.addEmitter(osg::Vec3f(), osg::Vec3f());
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

            layer.addEmitter(osg::Vec3f(), osg::Vec3f());
            for (float x : { 30.0f, 60.0f, 120.0f, 250.0f })
                for (int y = -12; y <= 12; ++y)
                    for (int z = -8; z <= 8; z += 2)
                        layer.addSprite(
                            osg::Vec3f(x, static_cast<float>(y) * 4.0f, static_cast<float>(z) * 3.0f), 1.5f);

            layer.addEmitter(osg::Vec3f(0.1f, 0.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, -1.0f));
            for (float x : { 25.0f, 75.0f, 150.0f })
                for (int y = -10; y <= 10; ++y)
                    layer.addSprite(osg::Vec3f(x, static_cast<float>(y) * 5.0f, 2.0f), 4.0f);

            layer.addEmitter(osg::Vec3f(), osg::Vec3f());
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
                layer.addEmitter(osg::Vec3f(), osg::Vec3f());

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
            layer.addEmitter(osg::Vec3f(), osg::Vec3f());
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
}
