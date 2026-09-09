#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Vec3f>

#include <components/rtx/shaders/scene.h>
#include <components/rtx/shaders/spriteshade.h>
#include <components/rtxvulkan/buffer.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/spriteshadepass.hpp>

#include "allocations.hpp"
#include "harness.hpp"

namespace Rtx
{
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
