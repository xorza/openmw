#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Math>
#include <osg/Matrixf>
#include <osg/Vec3f>

#include <components/rtx/camera.hpp>
#include <components/rtx/lightbuilder.hpp>
#include <components/rtx/mesh.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/look.h>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/shaders/visibility.h>
#include <components/rtx/sprite.hpp>
#include <components/rtx/spritelight.hpp>
#include <components/rtx/texturedata.hpp>
#include <components/rtx/texturewrap.hpp>
#include <components/vfs/pathutil.hpp>

#include "../geometry.hpp"
#include "../testcamera.hpp"
#include "../testtexture.hpp"
#include "fixture.hpp"

namespace Rtx::Testing
{
    namespace
    {
        /// A puff's texture is read across the disc the ray sees, so its cover ends where the
        /// texture's does and not at the disc's rim.
        ///
        /// **The disc is cut square to the ray and the texture was read along the screen's axes.**
        /// Those agree only on the frame's axis: a ray standing forty degrees off it saw the offset
        /// across the disc shortened to `cos 40° = 0.77` of itself, so the blob's edge at `13 / 16 =
        /// 0.81` of the texture lay beyond the silhouette, and the silhouette cut the blob's own
        /// alpha as a hard rim. Every puff in an ancestral tomb that stood away from the frame's
        /// centre wore a circle.
        ///
        /// A blob transparent beyond thirteen of its sixteen texels, on a puff thirty degrees off
        /// the axis of a frame ninety degrees wide, against the same puff wearing an opaque texel:
        /// walking out from the puff's centre toward the frame's edge, the opaque one covers the
        /// wall to the rim and the blob to `13 / 16` of the way there. Read along the screen's
        /// axes, the blob reached the rim too.
        TEST_F(RtxVisibilityTest, aPuffOffTheAxisIsReadAcrossTheDiscTheRaySees)
        {
            constexpr std::uint32_t size = 129;
            constexpr std::size_t row = size / 2;

            std::vector<std::uint8_t> texels(32 * 32 * 4);
            for (int y = 0; y < 32; ++y)
                for (int x = 0; x < 32; ++x)
                {
                    const float r = std::sqrt((x - 15.5f) * (x - 15.5f) + (y - 15.5f) * (y - 15.5f));
                    std::uint8_t* texel = &texels[(static_cast<std::size_t>(y) * 32 + static_cast<std::size_t>(x)) * 4];
                    texel[0] = texel[1] = texel[2] = 255;
                    texel[3] = r > 13.0f ? 0 : 255;
                }
            TestTexture blob;
            paintFlat(blob, 32, texels, "blob.dds");

            constexpr std::array<std::uint8_t, 4> opaque{ 255, 255, 255, 255 };

            // Thirty degrees off the axis, three hundred units out, at the height of the eye:
            // `300 sin 30° = 150` across and `300 cos 30° = 259.8` along. On a frame ninety degrees
            // wide the centre lands `tan 30° / tan 45°` of the half-frame from the middle.
            const float centre = static_cast<float>(row) * (1.0f + std::tan(osg::DegreesToRadians(30.0f)));

            // How far the puff's cover reaches from its centre toward the frame's edge, in pixels:
            // the last pixel of the row darker than the open wall.
            const auto coverReaches = [&](const TextureData& texture) {
                SceneDesc scene;
                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                    .mMesh
                    = scene.addMesh(MeshArrays{ .mPositions = sheetAt(4000.0f, 0.0f), .mIndices = sQuadIndices }) });
                const Index cut = scene.textures().add(VFS::Path::NormalizedView("blob.dds"), TextureWrap::Clamp);
                const std::array<Sprite, 1> sprites{ Sprite{ .mPosition = osg::Vec3f(150.0f, 259.81f, 200.0f),
                    .mRadius = 50.0f,
                    .mColour = osg::Vec3f(1.0f, 1.0f, 1.0f),
                    .mAlpha = 1.0f } };
                scene.addEmitter(sprites, cut, false);

                Shaders::VisibilityConstants camera = Testing::makeCamera(
                    osg::Vec3f(0.0f, 0.0f, 200.0f), osg::Vec3f(0.0f, 300.0f, 200.0f), 90.0f, size, size, 100000.0f);
                camera.mSkyHorizon = osg::Vec3f(0.6f, 0.6f, 0.6f);
                camera.mSkyZenith = camera.mSkyHorizon;
                camera.mAmbientFromSky = 1.0f;
                camera.mAmbient = osg::Vec3f();
                camera.mSun.mIrradiance = osg::Vec3f();

                const std::array<TextureData, 1> worn{ texture };
                std::vector<std::uint8_t> pixels;
                countHits(scene, std::span<const TextureData>(worn), camera, size, pixels);

                const float open = mRadiance[(row * size + 0) * 4];
                std::size_t last = 0;
                for (std::size_t x = static_cast<std::size_t>(centre); x < size; ++x)
                    if (mRadiance[(row * size + x) * 4] < open - 0.01f)
                        last = x;
                return static_cast<float>(last) + 0.5f - centre;
            };

            const float disc = coverReaches(describeTexel(opaque));
            const float painted = coverReaches(blob.mData);
            ASSERT_GT(disc, 10.0f) << "a disc wide enough to resolve the difference";

            // To the pixel the rim's own radius resolves, and the binary blob's edge is within one.
            EXPECT_NEAR(painted / disc, 13.0f / 16.0f, 1.5f / disc)
                << "the blob covers " << painted << " of a disc of " << disc;
        }

        /// A sprite is shadowed like anything else, by whatever stands over it.
        ///
        /// **A particle has no normal and it still has an up**, which is what the layer was missing:
        /// what a point sees of the sky is a question about the point. So rain under a bridge stops
        /// carrying the open sky, and smoke in a canyon stops carrying the full sun.
        ///
        /// **Two rays for the layer and not two for a puff.** `spritesAlong` asks at the first
        /// sprite that is lit, because a rainstorm puts dozens over a pixel and a ray apiece is what
        /// kept this unshadowed at all.
        ///
        /// **The lamps take the same treatment for the same reason**, and one more: what a lamp
        /// delivers runs as one over the square of a distance that changes from sprite to sprite,
        /// so the sum stays each puff's own and only the seeing is asked once for the layer.
        ///
        /// A lid four hundred units over the sprite and nothing else in the scene, and each source
        /// in turn: what lights the sprite in the open stops lighting it under the lid.
        ///
        /// **The two that arrive from a direction, and not the fill.** A puff has no side, so what
        /// it sees of an ambient is drawn over the whole sphere — and a lid over a sprite with
        /// nothing under it leaves the lower half of that sphere open, so the honest answer there is
        /// half and not nought. It is also one draw a froxel a frame, so a single frame of it is a
        /// coin: this test read whatever the seeds in `random.glsl` happened to make that coin say,
        /// and moving them by three took it from under a twentieth of the open value to nine tenths
        /// of it. What the fill owes to what stands near is measured exactly by the test below, in
        /// the room the reach makes it a question about.
        TEST_F(RtxVisibilityTest, aSpriteIsShadowedByWhatStandsOverIt)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);

            constexpr std::array<std::uint8_t, 4> white{ 255, 255, 255, 255 };
            const std::array<TextureData, 1> puff{ describeTexel(white) };

            // One source at a time, so what the lid takes is never ambiguous.
            enum class Source
            {
                Sun,
                Lamp,
            };

            const auto sprited = [&](bool lidded, Source source) {
                SceneDesc scene;
                const Index cut = scene.textures().add(VFS::Path::NormalizedView("sprite.dds"));
                const std::array<Sprite, 1> sprites{ Sprite{ .mPosition = osg::Vec3f(0.0f, 0.0f, 0.0f),
                    .mRadius = 60.0f,
                    .mColour = osg::Vec3f(1.0f, 1.0f, 1.0f),
                    .mAlpha = 1.0f } };
                scene.addEmitter(sprites, cut, false);

                if (lidded)
                    scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                        .mMesh = scene.addMesh(
                            MeshArrays{ .mPositions = sheetAt(4000.0f, 600.0f), .mIndices = sQuadIndices }) });

                // Over the lid, so the same sheet that takes the sun takes this too.
                if (source == Source::Lamp)
                    scene.addLight(Light{ .mPosition = osg::Vec3f(0.0f, 0.0f, 800.0f),
                        .mIntensity = osg::Vec3f(4.0e5f, 4.0e5f, 4.0e5f),
                        .mReach = 4000.0f });

                Shaders::VisibilityConstants camera = Testing::makeCamera(
                    osg::Vec3f(0.0f, -1.0f, 400.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);

                camera.mSkyHorizon = osg::Vec3f();
                camera.mSkyZenith = osg::Vec3f();
                camera.mAmbientFromSky = 1.0f;
                camera.mSun = Shaders::sunSource(
                    osg::Vec3f(0.0f, 0.0f, 1.0f), source == Source::Sun ? osg::Vec3f(4.0f, 4.0f, 4.0f) : osg::Vec3f());
                camera.mAmbient = osg::Vec3f();

                std::vector<std::uint8_t> pixels;
                countHits(scene, puff, camera, size, pixels);

                return mRadiance[centre];
            };

            // **Nought and not merely less**, which each of the two can be held to: a shadow ray at
            // the sun is one ray with no draw in it, and the lamp stands over the lid so every point
            // of it a ray is aimed at is behind the same sheet.
            for (const Source source : { Source::Sun, Source::Lamp })
            {
                const float open = sprited(false, source);
                ASSERT_GT(open, 0.01f) << "the source did not reach the sprite at all";

                EXPECT_EQ(sprited(true, source), 0.0f) << "a lid did not stop it";
            }
        }

        /// A sprite in front of the player's hand is looked for where the arms' ray crosses the
        /// world's picture, and not in the tile of the pixel the ray was cast for.
        ///
        /// **The bin is the world camera's, and the arms have an eye of their own.** Sixty-five
        /// pixels, the world at sixty degrees and the arms at thirty. Column 60's arms ray leaves
        /// at `u = 60.5 / 65 * 2 - 1 = 0.8615`, a slope of `0.8615 * tan 15° = 0.2309`, which the
        /// world's eye sees at `0.2309 / tan 30° = 0.400`: world pixel `1.4 / 2 * 65 = 45.5`, in
        /// tile 2, where column 60 is in tile 3. A ball of two a hundred ahead on that ray spans
        /// `2 / (100 tan 30°) * 32.5 = 1.13` pixels either side, 44.4 to 46.6, and with the bin's
        /// pixel of slack still inside tile 2. In front of a red first-person pane two hundred
        /// ahead, the ball is what the hand's pixel shows — its green is the ball's, since the pane
        /// has none — and without the ball the pixel is the pane.
        ///
        /// **And the composite has to know the pixel is the hand's.** The flag rode in a word the
        /// channel's format dropped, so the composite marched the world's ray there: the ball was
        /// looked for in the right tile and along the wrong line.
        TEST_F(RtxVisibilityTest, aSpriteInFrontOfTheArmsIsLookedUpWhereTheirRayCrossesTheWorldsPicture)
        {
            constexpr std::uint32_t size = 65;
            constexpr std::size_t column = 60;
            constexpr std::size_t pixel = std::size_t{ size / 2 } * size + column;

            constexpr std::array<std::uint8_t, 4> red{ 255, 0, 0, 255 };
            constexpr std::array<std::uint8_t, 4> white{ 255, 255, 255, 255 };
            const std::array<TextureData, 2> textures{ describeTexel(red, 0), describeTexel(white, 1) };

            const float slope
                = ((static_cast<float>(column) + 0.5f) / size * 2.0f - 1.0f) * std::tan(osg::DegreesToRadians(15.0f));

            const auto seenAt = [&](bool sprited) {
                SceneDesc scene;
                const Index pane = scene.textures().add(VFS::Path::NormalizedView("red.dds"));
                const Index puff = scene.textures().add(VFS::Path::NormalizedView("white.dds"));

                const float across = 200.0f * slope;
                const std::array<osg::Vec3f, 4> corners{
                    osg::Vec3f(across - 10.0f, 100.0f, -10.0f),
                    osg::Vec3f(across + 10.0f, 100.0f, -10.0f),
                    osg::Vec3f(across + 10.0f, 100.0f, 10.0f),
                    osg::Vec3f(across - 10.0f, 100.0f, 10.0f),
                };
                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                    .mMesh = scene.addMesh(
                        MeshArrays{ .mPositions = corners, .mTexCoords = sQuadUv, .mIndices = sQuadIndices }),
                    .mMaterial = scene.addMaterial(Material{ .mDiffuse = pane }),
                    .mClass = InstanceClass::FirstPerson });

                if (sprited)
                {
                    const std::array<Sprite, 1> sprites{ Sprite{ .mPosition = osg::Vec3f(100.0f * slope, 0.0f, 0.0f),
                        .mRadius = 2.0f,
                        .mColour = osg::Vec3f(1.0f, 1.0f, 1.0f),
                        .mAlpha = 1.0f } };
                    scene.addEmitter(sprites, puff, false);
                }

                Shaders::VisibilityConstants camera = Testing::makeCamera(
                    osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);
                camera.mArms = cameraAtFieldOfView(camera.mCamera, 30.0f);
                camera.mSkyHorizon = osg::Vec3f();
                camera.mSkyZenith = osg::Vec3f();
                camera.mSun.mIrradiance = osg::Vec3f();
                camera.mAmbient = osg::Vec3f(0.5f, 0.5f, 0.5f);
                camera.mAmbientFromSky = 0.0f;

                // The surfaces as their albedo, so the pane is red and nothing else; the ball is lit
                // by the fill, which the composite puts over the albedo as it puts it over a frame.
                // The last of three frames: the first bin has no report to size its list by, walks
                // every sprite unbinned, and would find the ball from any tile.
                std::vector<std::uint8_t> pixels;
                countHits(scene, textures, camera, size, pixels,
                    Shot{ .mFrames = 3, .mAverage = false, .mShow = SurfaceView::Albedo });

                return std::array<float, 2>{ mRadiance[pixel * 4], mRadiance[pixel * 4 + 1] };
            };

            const std::array<float, 2> bare = seenAt(false);
            EXPECT_EQ(bare[0], 1.0f) << "the hand's pixel is not the pane";
            EXPECT_EQ(bare[1], 0.0f);
            EXPECT_GT(seenAt(true)[1], 0.25f) << "the sprite in front of the hand was not found";
        }

        /// A room's fill reaches a puff from every side, and what stands near takes it away.
        ///
        /// **A puff is a point in a medium and has no face to turn away from**, so what it sees of
        /// an `AMBI` fill is a question about the whole sphere rather than about a hemisphere. It
        /// used to be no question at all: indoors the layer asked the world nothing and a puff's own
        /// thickness was the whole answer, so smoke under a table came out as bright as smoke in the
        /// middle of the floor.
        ///
        /// **The law is exactly linear, which is what makes this an assertion rather than a
        /// comparison.** A sheet `h` above and another `h` below block every direction that reaches
        /// them inside `ROOM_FILL_REACH` — that is `|d.z| >= h / reach` — and a uniform sphere draw
        /// puts `d.z` evenly on `[-1, 1]`, so the share left is `h / reach` and nothing else. At 70
        /// and 105 units of a 140-unit reach that is a half and three quarters of the open fill.
        ///
        /// **And it is the sphere the law comes from.** Drawing the cosine about the up instead
        /// would leave `(h / reach)^2` — a quarter and nine sixteenths — which is 0.25 and 0.19 from
        /// the law, more than twice the tolerance below.
        ///
        /// **Not nearer, because the puff is lit out of the fog's columns.** A column is eight
        /// pixels square, forty units at the puff, and a sheet 35 units off cuts through the columns
        /// the middle row reads: what the field holds there is the column's mean over its block,
        /// which is not the point's — measured with independent draws over 1024 frames, 0.35 for a
        /// law of 0.25. **And the tolerance is the field's own noise at 128 frames**: four runs
        /// from four starting frames came within 0.05 of the law at both heights.
        ///
        /// The sprite is opaque and carries no lighting bake, so what a pixel shows is the fill
        /// alone. Only the middle row is read: those rays are level, so they meet neither sheet and
        /// the chord they cut is the same one in all three scenes.
        TEST_F(RtxVisibilityTest, aRoomsFillReachesAPuffFromEverySideAndWhatIsNearTakesItAway)
        {
            constexpr std::uint32_t size = 33;
            constexpr float reach = 140.0f;

            constexpr std::array<std::uint8_t, 4> white{ 255, 255, 255, 255 };
            const std::array<TextureData, 1> puff{ describeTexel(white) };

            const auto boxedAt = [&](float half) {
                SceneDesc scene;
                const Index cut = scene.textures().add(VFS::Path::NormalizedView("sprite.dds"));
                const std::array<Sprite, 1> sprites{ Sprite{ .mPosition = osg::Vec3f(0.0f, 0.0f, 0.0f),
                    .mRadius = 40.0f,
                    .mColour = osg::Vec3f(1.0f, 1.0f, 1.0f),
                    .mAlpha = 1.0f } };
                scene.addEmitter(sprites, cut, false);

                // Nothing at all where the fill is whole, rather than sheets moved out of reach: a
                // scene with no geometry is the one case where the answer cannot be the geometry's.
                if (half > 0.0f)
                    for (const float z : { half, -half })
                        scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                            .mMesh = scene.addMesh(
                                MeshArrays{ .mPositions = sheetAt(4000.0f, z), .mIndices = sQuadIndices }) });

                Shaders::VisibilityConstants camera = Testing::makeCamera(
                    osg::Vec3f(0.0f, -reach, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);

                camera.mSkyHorizon = osg::Vec3f();
                camera.mSkyZenith = osg::Vec3f();
                camera.mSun.mIrradiance = osg::Vec3f();
                camera.mAmbient = osg::Vec3f(0.5f, 0.5f, 0.5f);
                camera.mAmbientFromSky = 0.0f;

                std::vector<std::uint8_t> pixels;
                countHits(scene, puff, camera, size, pixels, { .mFrames = 128 });

                // The middle row, whose rays leave the eye level and stay level.
                float sum = 0.0f;
                for (std::uint32_t x = 0; x < size; ++x)
                    sum += mRadiance[(std::size_t{ size / 2 } * size + x) * 4];

                return sum;
            };

            const float open = boxedAt(0.0f);
            ASSERT_GT(open, 0.01f) << "the fill did not light the puff at all";

            EXPECT_NEAR(boxedAt(70.0f) / open, 70.0f / reach, 0.08f) << "half of the sphere is left";
            EXPECT_NEAR(boxedAt(105.0f) / open, 105.0f / reach, 0.08f) << "and three quarters in a taller room";
        }

        /// The alpha every sprite test below cuts its sprite from: half, so that what it hides and
        /// what it lets through are the same size and neither can pass by being nought or one.
        constexpr float sHalfAlpha = 128.0f / 255.0f;

        /// A sprite is a ball the ray crosses, so a floor cuts its chord rather than clipping its disc.
        ///
        /// The eye looks along a line through the sprite's centre. Hanging in the open, the whole
        /// chord is seen and the sprite hides exactly what was painted — `128/255`, leaving
        /// `0.49804` of the floor behind it. Centred on the floor, the ray meets the floor at the
        /// centre and sees half the chord, which leaves `0.49804^0.5 = 0.70572`. The puff is unlit —
        /// no ambient, no sun, no lamp — so what the pixel shows is the floor through it and nothing
        /// else, and the floor alone is measured in the same pose to divide by.
        TEST_F(RtxVisibilityTest, aSpriteIsAChordAndAFloorCutsItInHalf)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);

            constexpr std::array<std::uint8_t, 4> half{ 255, 255, 255, 128 };
            const std::array<TextureData, 1> puff{ describeTexel(half) };

            const auto through = [&](float height, bool sprited) {
                SceneDesc scene;
                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                    .mMesh
                    = scene.addMesh(MeshArrays{ .mPositions = sheetAt(4000.0f, 0.0f), .mIndices = sQuadIndices }) });

                if (sprited)
                {
                    const Index cut = scene.textures().add(VFS::Path::NormalizedView("sprite.dds"));
                    const std::array<Sprite, 1> sprites{ Sprite{ .mPosition = osg::Vec3f(0.0f, 0.0f, height),
                        .mRadius = 60.0f,
                        .mColour = osg::Vec3f(1.0f, 1.0f, 1.0f),
                        .mAlpha = 1.0f } };
                    scene.addEmitter(sprites, cut, false);
                }

                // Forty up over four hundred along, so the centre ray runs down through the
                // sprite's centre and on to the floor, near or far.
                Shaders::VisibilityConstants camera = Testing::makeCamera(osg::Vec3f(0.0f, -400.0f, height + 40.0f),
                    osg::Vec3f(0.0f, 0.0f, height), 60.0f, size, size, 100000.0f);

                // An even sky lights the floor exactly and the puff not at all: a puff is lit by the
                // ambient, the sun and the lamps, and none of those is here.
                camera.mSkyHorizon = osg::Vec3f(0.6f, 0.6f, 0.6f);
                camera.mSkyZenith = camera.mSkyHorizon;
                camera.mAmbientFromSky = 1.0f;
                camera.mAmbient = osg::Vec3f();
                camera.mSun.mIrradiance = osg::Vec3f();

                std::vector<std::uint8_t> pixels;
                countHits(scene, sprited ? std::span<const TextureData>(puff) : std::span<const TextureData>(), camera,
                    size, pixels);

                return mRadiance[centre];
            };

            const float openFloor = through(200.0f, false);
            const float buriedFloor = through(0.0f, false);
            ASSERT_GT(openFloor, 0.01f) << "the floor is not lit";
            ASSERT_GT(buriedFloor, 0.01f) << "the floor is not lit";

            EXPECT_NEAR(through(200.0f, true) / openFloor, 1.0f - sHalfAlpha, 0.005f)
                << "a whole chord hides exactly what was painted";
            EXPECT_NEAR(through(0.0f, true) / buriedFloor, std::sqrt(1.0f - sHalfAlpha), 0.005f)
                << "half a chord lets through the square root";
        }

        /// Two flames in one place add less than twice one, because a flame absorbs what it emits.
        ///
        /// A texel of alpha `128/255 = 0.50196` adds that share of `SUNLIT_WHITE` on its own,
        /// which is what it always added. Two of them screen — `1 - 0.49804^2 = 0.75196` of it —
        /// where a sum would have reached `1.00392`. The original's framebuffer clamped that sum at
        /// one, and this is the smooth form of the same limit.
        TEST_F(RtxVisibilityTest, flamesSaturateWhereTheOriginalClamped)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);

            constexpr std::array<std::uint8_t, 4> half{ 255, 255, 255, 128 };
            const std::array<TextureData, 1> flame{ describeTexel(half) };

            const auto glowing = [&](std::size_t count) {
                SceneDesc scene;
                const Index cut = scene.textures().add(VFS::Path::NormalizedView("sprite.dds"));
                const std::vector<Sprite> flames(count,
                    Sprite{ .mPosition = osg::Vec3f(0.0f, 0.0f, 0.0f),
                        .mRadius = 60.0f,
                        .mColour = osg::Vec3f(1.0f, 1.0f, 1.0f),
                        .mAlpha = 1.0f });
                scene.addEmitter(flames, cut, true);

                Shaders::VisibilityConstants camera = Testing::makeCamera(
                    osg::Vec3f(0.0f, -400.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
                camera.mSkyHorizon = osg::Vec3f();
                camera.mSkyZenith = osg::Vec3f();
                camera.mAmbient = osg::Vec3f();
                camera.mSun.mIrradiance = osg::Vec3f();

                std::vector<std::uint8_t> pixels;
                countHits(scene, flame, camera, size, pixels);

                return mRadiance[centre];
            };

            EXPECT_NEAR(glowing(1), Shaders::SUNLIT_WHITE * sHalfAlpha, 0.01f) << "one adds what it painted";
            EXPECT_NEAR(glowing(2), Shaders::SUNLIT_WHITE * (1.0f - (1.0f - sHalfAlpha) * (1.0f - sHalfAlpha)), 0.01f)
                << "two screen rather than sum";
        }

        /// A streak hangs on the axis its own particle carries, and the trace draws it leaning.
        ///
        /// **The axis is the sprite's and not the emitter's**, because `osgParticle` turns a quad by
        /// the angle the particle holds and `Weather::RainShooter` leans every drop it fires into
        /// the wind that way. The march read the emitter's own authored axis for as long as there
        /// was one, so a storm the rasterizer drew leaning fell straight down here.
        ///
        /// A streak 120 long and a quarter of that wide, seen face-on from 400 units off through
        /// sixty degrees — so half the frame is `400 * tan 30 = 230.94` units at the sprite's own
        /// plane. Standing upright it covers `|x| < 30` about a fall of `|z| < 120`; leant
        /// forty-five degrees it covers that rectangle turned, its length running up and to the
        /// left. A point eighty above the middle is inside the first and `80 * sin 45 = 56.6` from
        /// the second's axis, which is well outside its thirty — and a point eighty up the leant
        /// axis is the other way about.
        ///
        /// Additive, so what a pixel holds is the coverage alone and nothing has to light it.
        TEST_F(RtxVisibilityTest, aStreakLeansWithTheAxisItsOwnParticleCarries)
        {
            constexpr std::uint32_t size = 65;
            constexpr float radius = 120.0f;
            constexpr float width = 0.25f;

            constexpr std::array<std::uint8_t, 4> white{ 255, 255, 255, 255 };
            const std::array<TextureData, 1> drop{ describeTexel(white) };

            const auto drawn = [&](const osg::Vec3f& axis) {
                SceneDesc scene;
                const Index cut = scene.textures().add(VFS::Path::NormalizedView("sprite.dds"));
                const std::array<Sprite, 1> sprites{ Sprite{ .mPosition = osg::Vec3f(0.0f, 0.0f, 0.0f),
                    .mRadius = radius,
                    .mAxis = axis,
                    .mColour = osg::Vec3f(1.0f, 1.0f, 1.0f),
                    .mAlpha = 1.0f } };
                scene.addEmitter(sprites, cut, true, width);

                Shaders::VisibilityConstants camera = Testing::makeCamera(
                    osg::Vec3f(0.0f, -400.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
                camera.mSkyHorizon = osg::Vec3f();
                camera.mSkyZenith = osg::Vec3f();
                camera.mAmbient = osg::Vec3f();
                camera.mSun.mIrradiance = osg::Vec3f();

                std::vector<std::uint8_t> pixels;
                countHits(scene, drop, camera, size, pixels);

                return mRadiance;
            };

            // Where a point of the sprite's own plane lands, as the first value of its pixel. The
            // middle of pixel `n` sits at `(n + 0.5) / size * 2 - 1` of half the frame, and the
            // frame's up is the screen's down.
            constexpr float half = 230.9401f;
            const auto pixelAt = [](float x, float z) {
                const auto column = static_cast<std::uint32_t>(std::lround((x / half + 1.0f) * 0.5f * size - 0.5f));
                const auto row = static_cast<std::uint32_t>(std::lround((-z / half + 1.0f) * 0.5f * size - 0.5f));

                return (std::size_t{ row } * size + column) * 4;
            };

            constexpr float lean = 0.70710678f;
            const std::size_t aboveTheMiddle = pixelAt(0.0f, 80.0f);
            const std::size_t upTheLean = pixelAt(-80.0f * lean, 80.0f * lean);

            const std::vector<float> upright = drawn(osg::Vec3f(0.0f, 0.0f, -1.0f));
            const std::vector<float> leaning = drawn(osg::Vec3f(lean, 0.0f, -lean));

            ASSERT_EQ(upright.size(), std::size_t{ size } * size * 4);

            EXPECT_GT(upright[aboveTheMiddle], 0.01f) << "a streak standing up did not cover its own fall";
            EXPECT_EQ(upright[upTheLean], 0.0f) << "a streak standing up covered the diagonal";

            EXPECT_EQ(leaning[aboveTheMiddle], 0.0f) << "a leaning streak still covered the upright fall";
            EXPECT_GT(leaning[upTheLean], 0.01f) << "the lean the particle carries never reached the picture";

            // **A turn cannot change the shape it turned**, so the leaning streak is measured along
            // its new axis and across it: 120 long and 30 wide, exactly as it stood.
            //
            // A hundred up the lean is inside its fall and a hundred and forty is past the end of
            // it. Twenty across, taken sixty up, is inside its width and forty-five is outside —
            // and every one of those is several pixels clear of the edge it names.
            const auto alongLean
                = [&](float along, float across) { return pixelAt(lean * (across - along), lean * (along + across)); };

            EXPECT_GT(leaning[alongLean(100.0f, 0.0f)], 0.01f) << "the streak fell short of its own length";
            EXPECT_EQ(leaning[alongLean(140.0f, 0.0f)], 0.0f) << "the streak ran past its own length";
            EXPECT_GT(leaning[alongLean(60.0f, 20.0f)], 0.01f) << "the streak was narrower than its width";
            EXPECT_EQ(leaning[alongLean(60.0f, 45.0f)], 0.0f) << "the streak was wider than its width";
        }

        /// A streak takes the sun broadside and takes nothing along its own axis.
        ///
        /// **The march draws an oriented quad as a cylinder, so it is lit as one.** Its width is
        /// swung about its axis to meet the ray, which is a cylinder's silhouette, and such a body
        /// presents `sin` of the angle off its axis to whatever lights it. A streak used to take a
        /// full card's worth of the sun whichever way it hung, so rain at noon was lit as brightly
        /// as rain at dawn.
        ///
        /// One sun, straight up, and the streak turned under it — which is what keeps everything
        /// else about the three frames identical: the same coverage, the same level, the same air.
        /// Lying across the sun it takes the whole of the broadside; thirty degrees off the vertical
        /// takes `sin 30`, which is half of it; straight up and down takes none.
        TEST_F(RtxVisibilityTest, aStreakTakesTheSunBroadsideAndNothingAlongItsOwnAxis)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);

            constexpr std::array<std::uint8_t, 4> white{ 255, 255, 255, 255 };
            const std::array<TextureData, 1> drop{ describeTexel(white) };

            const auto lit = [&](const osg::Vec3f& axis) {
                SceneDesc scene;
                const Index cut = scene.textures().add(VFS::Path::NormalizedView("sprite.dds"));
                const std::array<Sprite, 1> sprites{ Sprite{ .mPosition = osg::Vec3f(0.0f, 0.0f, 0.0f),
                    .mRadius = 60.0f,
                    .mAxis = axis,
                    .mColour = osg::Vec3f(1.0f, 1.0f, 1.0f),
                    .mAlpha = 1.0f } };
                scene.addEmitter(sprites, cut, false, 0.25f);

                Shaders::VisibilityConstants camera = Testing::makeCamera(
                    osg::Vec3f(0.0f, -400.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);

                // The sun and nothing else, so what a pixel holds is the shape's own share of it.
                camera.mSkyHorizon = osg::Vec3f();
                camera.mSkyZenith = osg::Vec3f();
                camera.mAmbient = osg::Vec3f();
                camera.mSun = Shaders::sunSource(osg::Vec3f(0.0f, 0.0f, 1.0f), osg::Vec3f(4.0f, 4.0f, 4.0f));

                std::vector<std::uint8_t> pixels;
                countHits(scene, drop, camera, size, pixels);

                return mRadiance[centre];
            };

            constexpr float lean = 0.8660254f;
            const float across = lit(osg::Vec3f(1.0f, 0.0f, 0.0f));
            const float leaning = lit(osg::Vec3f(0.5f, 0.0f, -lean));
            const float upright = lit(osg::Vec3f(0.0f, 0.0f, -1.0f));

            ASSERT_GT(across, 0.01f) << "a streak lying across the sun was not lit at all";

            EXPECT_NEAR(leaning / across, 0.5f, 0.02f) << "thirty degrees off the sun is sin 30 of it";
            EXPECT_NEAR(upright, 0.0f, across * 0.02f) << "a streak pointing at the sun has no side to turn to it";
        }

        /// The level a streak is read at comes from the axis its texels are densest along.
        ///
        /// **A rain streak carries its texture at two densities**, because the quad is a fraction as
        /// wide as it is long and the texture is stretched over the whole of it. A level chosen from
        /// the length alone reads the width sharper than the ray can carry, which is the drop
        /// aliasing into a hard mark rather than fading — and it is what a drop did at every
        /// distance in the game.
        ///
        /// **Two levels painted different colours, so which one was read is in the picture.** The
        /// texture is four square and the sprite is sixty across. A quad as wide as it is long
        /// spreads four texels over a hundred and twenty units either way, one every thirty; a quad
        /// a quarter as wide spreads the same four over thirty, one every seven and a half. The
        /// pixel's cone at eight hundred units is `atan(2 tan 30 / 33) * 800 = 27.98` units, which
        /// is under one texel of the first and 3.73 of the second — so the first reads level nought
        /// and the second reads the level below it.
        TEST_F(RtxVisibilityTest, theLevelAStreakIsReadAtComesFromTheAxisItsTexelsAreDensestAlong)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);

            // Level nought is white and level one is red, so the green channel says which was read.
            TestTexture layered;
            for (std::uint32_t texel = 0; texel < 4 * 4; ++texel)
                for (const std::uint8_t byte : { 255, 255, 255, 255 })
                    layered.mBytes.push_back(byte);

            for (std::uint32_t texel = 0; texel < 2 * 2; ++texel)
                for (const std::uint8_t byte : { 255, 0, 0, 255 })
                    layered.mBytes.push_back(byte);

            layered.mLevels.push_back(MipLevel{ .mOffset = 0, .mWidth = 4, .mHeight = 4 });
            layered.mLevels.push_back(MipLevel{ .mOffset = 4 * 4 * 4, .mWidth = 2, .mHeight = 2 });
            layered.describe(4, 4, "layered.dds");

            const auto green = [&](float width) {
                SceneDesc scene;
                const Index cut = scene.textures().add(VFS::Path::NormalizedView("sprite.dds"));
                const std::array<Sprite, 1> sprites{ Sprite{ .mPosition = osg::Vec3f(0.0f, 0.0f, 0.0f),
                    .mRadius = 60.0f,
                    .mAxis = osg::Vec3f(0.0f, 0.0f, -1.0f),
                    .mColour = osg::Vec3f(1.0f, 1.0f, 1.0f),
                    .mAlpha = 1.0f } };

                // Additive, so what a pixel holds is the texel it read and nothing has to light it.
                scene.addEmitter(sprites, cut, true, width);

                Shaders::VisibilityConstants camera = Testing::makeCamera(
                    osg::Vec3f(0.0f, -800.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
                camera.mSkyHorizon = osg::Vec3f();
                camera.mSkyZenith = osg::Vec3f();
                camera.mAmbient = osg::Vec3f();
                camera.mSun.mIrradiance = osg::Vec3f();

                std::vector<std::uint8_t> pixels;
                countHits(scene, std::span(&layered.mData, 1), camera, size, pixels);

                return mRadiance[centre + 1] / std::max(mRadiance[centre], 1.0e-6f);
            };

            EXPECT_NEAR(green(1.0f), 1.0f, 0.05f) << "a square quad read the level below its own";
            EXPECT_NEAR(green(0.25f), 0.0f, 0.05f) << "a streak read its length rather than its width";
        }

        /// A puff is lit from the side the light is on, and by what its own texture lets through.
        ///
        /// A sprite facing an eye that looks along +Y, lit by a sun of four and nothing else, so its
        /// centre pixel is `4 / pi` per unit of albedo, times what the puff lets through, times its
        /// half alpha: `0.6391` for a puff nothing shadows. At the centre the ball's normal is
        /// toward the eye, so a sun to the side is at the mean — and the bake alone decides: a sun
        /// from the screen's right is `+u`, from above is `+v`, and a channel of nought there puts
        /// the sun out, while the mirror channel is left alone.
        ///
        /// **And the ball has a side.** A sun behind the eye lights the near side at `1 + SPRITE_WRAP`
        /// through a front nothing shadows; one behind the sprite lights it at `1 - SPRITE_WRAP`
        /// through the whole of the texel's thickness, which is `1 - alpha`.
        ///
        /// **And the sun is thrown forward.** Henyey-Greenstein at `g = 0.6` against the even share
        /// is `(1 - g^2) / (1 + g^2 - 2 g cos)^1.5 = 0.64 / (1.36 - 1.2 cos)^1.5`: from the side
        /// `0.64 / 1.36^1.5 = 0.40353`, from behind the eye `0.64 / 2.56^1.5 = 0.15625`, and from
        /// behind the sprite `0.64 / 0.16^1.5 = 10`. The side cases carry the first, and the two
        /// sides of the ball the other two — which is what makes them differ by sixty-four and not
        /// by six.
        ///
        /// **A bake that stands in is no bake**: the puff is lit as a card, where the stand-in's grey
        /// would shut the sun to a quarter.
        TEST_F(RtxVisibilityTest, aPuffIsLitByItsSideAndByWhatItsTextureLetsThrough)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);

            constexpr std::array<std::uint8_t, 4> half{ 255, 255, 255, 128 };

            const auto lit = [&](const osg::Vec3f& sun, std::array<std::uint8_t, 4> through, bool standsIn = false) {
                std::array<TextureData, 2> textures{ describeTexel(half), describeTexel(through) };
                if (standsIn)
                    textures[1].mSource = TextureSource::StandIn;

                SceneDesc scene;
                const Index cut = scene.textures().add(VFS::Path::NormalizedView("sprite.dds"));
                const Index bake
                    = scene.textures().addBaked(SpriteLightMap::keyFor(VFS::Path::NormalizedView("sprite.dds")));
                const std::array<Sprite, 1> sprites{ Sprite{ .mPosition = osg::Vec3f(0.0f, 0.0f, 0.0f),
                    .mRadius = 60.0f,
                    .mColour = osg::Vec3f(1.0f, 1.0f, 1.0f),
                    .mAlpha = 1.0f } };
                scene.addEmitter(sprites, cut, false, 0.0f, bake);

                Shaders::VisibilityConstants camera = Testing::makeCamera(
                    osg::Vec3f(0.0f, -400.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
                camera.mSkyHorizon = osg::Vec3f();
                camera.mSkyZenith = osg::Vec3f();
                camera.mAmbient = osg::Vec3f();
                camera.mAmbientFromSky = 1.0f;
                camera.mSun = Shaders::sunSource(sun, osg::Vec3f(4.0f, 4.0f, 4.0f));

                std::vector<std::uint8_t> pixels;
                countHits(scene, textures, camera, size, pixels);

                return mRadiance[centre];
            };

            const float card = 4.0f * Shaders::INV_PI * sHalfAlpha;
            constexpr float sideways = 0.40353f;
            constexpr float backward = 0.15625f;
            constexpr float forward = 10.0f;
            constexpr std::array<std::uint8_t, 4> clear{ 255, 255, 255, 255 };

            EXPECT_NEAR(lit(osg::Vec3f(1.0f, 0.0f, 0.0f), clear), card * sideways, 0.005f)
                << "a card's worth from the side, thrown";
            EXPECT_NEAR(lit(osg::Vec3f(1.0f, 0.0f, 0.0f), { 0, 255, 255, 255 }), 0.0f, 0.005f)
                << "the sun from +u, and +u shut";
            EXPECT_NEAR(lit(osg::Vec3f(1.0f, 0.0f, 0.0f), { 0, 255, 255, 255 }, true), card * sideways, 0.005f)
                << "a bake that stands in, read as the card it is without one";
            EXPECT_NEAR(lit(osg::Vec3f(-1.0f, 0.0f, 0.0f), { 0, 255, 255, 255 }), card * sideways, 0.005f)
                << "the sun from -u, which +u does not shut";
            EXPECT_NEAR(lit(osg::Vec3f(0.0f, 0.0f, 1.0f), { 255, 255, 0, 255 }), 0.0f, 0.005f)
                << "the sun from above is +v, and +v shut";

            EXPECT_NEAR(
                lit(osg::Vec3f(0.0f, -1.0f, 0.0f), clear), card * (1.0f + Shaders::SPRITE_WRAP) * backward, 0.005f)
                << "the near side, through the front, thrown away from the eye";
            EXPECT_NEAR(lit(osg::Vec3f(0.0f, 1.0f, 0.0f), clear),
                card * (1.0f - Shaders::SPRITE_WRAP) * (1.0f - sHalfAlpha) * forward, 0.02f)
                << "the far side, through the thickness, thrown toward the eye";
        }

        /// A puff in the shade of its own emitter is thinned by what one layer of its texture hides.
        ///
        /// Two puffs of one emitter, the second a hundred units toward a sun from the side and sixty
        /// in radius, so the first's path to the sun runs through it and the eye's ray to the first
        /// does not. `spriteshade.h` counts one whole layer, and the shader thins the sun by the
        /// texture's mean alpha — its one texel, `128/255` — to `0.49804` of the card's worth from
        /// the side. Nothing stands over either, so the sky is untouched, and the ambient is nought.
        TEST_F(RtxVisibilityTest, aPuffInTheShadeOfItsOwnEmitterIsThinnedByOneLayer)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);

            constexpr std::array<std::uint8_t, 4> half{ 255, 255, 255, 128 };
            const std::array<TextureData, 1> puff{ describeTexel(half) };

            const auto lit = [&](bool shaded) {
                SceneDesc scene;
                const Index cut = scene.textures().add(VFS::Path::NormalizedView("sprite.dds"));
                std::vector<Sprite> sprites{ Sprite{ .mPosition = osg::Vec3f(0.0f, 0.0f, 0.0f),
                    .mRadius = 60.0f,
                    .mColour = osg::Vec3f(1.0f, 1.0f, 1.0f),
                    .mAlpha = 1.0f } };
                if (shaded)
                    sprites.push_back(Sprite{ .mPosition = osg::Vec3f(100.0f, 0.0f, 0.0f),
                        .mRadius = 60.0f,
                        .mColour = osg::Vec3f(1.0f, 1.0f, 1.0f),
                        .mAlpha = 1.0f });
                scene.addEmitter(sprites, cut, false);

                Shaders::VisibilityConstants camera = Testing::makeCamera(
                    osg::Vec3f(0.0f, -400.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
                camera.mSkyHorizon = osg::Vec3f();
                camera.mSkyZenith = osg::Vec3f();
                camera.mAmbient = osg::Vec3f();
                camera.mAmbientFromSky = 1.0f;
                camera.mSun = Shaders::sunSource(osg::Vec3f(1.0f, 0.0f, 0.0f), osg::Vec3f(4.0f, 4.0f, 4.0f));

                std::vector<std::uint8_t> pixels;
                countHits(scene, puff, camera, size, pixels);

                return mRadiance[centre];
            };

            const float alone = lit(false);
            ASSERT_GT(alone, 0.1f) << "the sun did not reach the puff at all";
            EXPECT_NEAR(lit(true) / alone, 1.0f - sHalfAlpha, 0.01f) << "one layer of a half-alpha texture";
        }

        /// A drop under a roof is not drawn, and a hearth's smoke under the same roof is.
        ///
        /// **The rasterizer's occluder, as a ray.** `PrecipitationOccluder` discards every drop
        /// with a static over it inside its box; `spriteshelter.rgen` traces every falling sprite
        /// straight up to the top of that box and zeroes the one that meets a surface. Two drops
        /// of one falling emitter, a lid over the left one alone, and a sun from behind the eye so
        /// the lid shadows neither — with shelter in the frame the left pixel shows nothing at all
        /// and the right one is what it was, and with none in the frame both are drawn. The same
        /// two under a lid, from an emitter that does not fall, stay where they are.
        ///
        /// **A frame with no shelter height in it and not a flag**: the game says nought where
        /// what falls is ash, and a picture inside the interface says nought because it has no
        /// world over it. Nought is what keeps the launch off nearly every frame.
        TEST_F(RtxVisibilityTest, aRoofKeepsTheRainOffAndLeavesTheSmoke)
        {
            constexpr std::uint32_t size = 33;

            // At thirty degrees a thousand units out the frame is 536 units wide, so a drop a
            // hundred units off the axis lands six pixels from the middle, and its radius of forty
            // covers two and a half. The middle row, since the drops stand at the eye's height.
            constexpr std::size_t row = size / 2;
            constexpr std::size_t left = (row * size + 10) * 4;
            constexpr std::size_t right = (row * size + 22) * 4;

            constexpr std::array<std::uint8_t, 4> white{ 255, 255, 255, 255 };
            const std::array<TextureData, 1> puff{ describeTexel(white) };

            struct Read
            {
                float mLeft;
                float mRight;
            };

            const auto shown = [&](bool falls, float shelter) {
                SceneDesc scene;
                const Index cut = scene.textures().add(VFS::Path::NormalizedView("sprite.dds"));
                const std::array<Sprite, 2> drops{
                    Sprite{ .mPosition = osg::Vec3f(-100.0f, 0.0f, 0.0f),
                        .mRadius = 40.0f,
                        .mColour = osg::Vec3f(1.0f, 1.0f, 1.0f),
                        .mAlpha = 1.0f },
                    Sprite{ .mPosition = osg::Vec3f(100.0f, 0.0f, 0.0f),
                        .mRadius = 40.0f,
                        .mColour = osg::Vec3f(1.0f, 1.0f, 1.0f),
                        .mAlpha = 1.0f },
                };
                scene.addEmitter(drops, cut, false, 0.0f, sNoIndex, falls);

                // A lid two hundred wide over the left drop alone, three hundred up.
                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::translate(-100.0f, 0.0f, 0.0f),
                    .mMesh
                    = scene.addMesh(MeshArrays{ .mPositions = sheetAt(100.0f, 300.0f), .mIndices = sQuadIndices }) });

                Shaders::VisibilityConstants camera = Testing::makeCamera(
                    osg::Vec3f(0.0f, -1000.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 30.0f, size, size, 100000.0f);
                camera.mSkyHorizon = osg::Vec3f();
                camera.mSkyZenith = osg::Vec3f();
                camera.mAmbient = osg::Vec3f();
                camera.mAmbientFromSky = 1.0f;
                camera.mSun = Shaders::sunSource(osg::Vec3f(0.0f, -1.0f, 0.0f), osg::Vec3f(4.0f, 4.0f, 4.0f));
                camera.mShelterHeight = shelter;

                std::vector<std::uint8_t> pixels;
                countHits(scene, puff, camera, size, pixels);

                return Read{ mRadiance[left], mRadiance[right] };
            };

            const Read open = shown(true, 0.0f);
            ASSERT_GT(open.mLeft, 0.1f) << "the left drop was not drawn at all";
            ASSERT_GT(open.mRight, 0.1f) << "the right drop was not drawn at all";

            const Read sheltered = shown(true, 2000.0f);
            EXPECT_EQ(sheltered.mLeft, 0.0f) << "the roof did not keep the rain off";
            EXPECT_EQ(sheltered.mRight, open.mRight) << "the shelter reached a drop with nothing over it";

            const Read smoke = shown(false, 2000.0f);
            EXPECT_EQ(smoke.mLeft, open.mLeft) << "the roof took a hearth's smoke";
            EXPECT_EQ(smoke.mRight, open.mRight);
        }
    }
}
