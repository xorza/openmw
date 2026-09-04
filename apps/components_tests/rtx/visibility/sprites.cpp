#include "fixture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Rtx::Testing
{
    namespace
    {
        /// A one-texel sprite texture: white, and as opaque as its fourth byte says.
        struct OneTexel
        {
            std::array<std::uint8_t, 4> mBytes;
            MipLevel mLevel{ 0, 1, 1 };

            explicit OneTexel(std::array<std::uint8_t, 4> bytes)
                : mBytes(bytes)
            {
            }

            TextureData describe() const
            {
                return TextureData{
                    .mFormat = TextureFormat::Rgba8Unorm,
                    .mWidth = 1,
                    .mHeight = 1,
                    .mBytes = std::as_bytes(std::span(mBytes)),
                    .mLevels = std::span(&mLevel, 1),
                };
            }
        };

        /// A sprite is shadowed like anything else, by the sun and by the sky over it.
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
        TEST_F(RtxVisibilityTest, aSpriteIsShadowedByWhatStandsOverIt)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;

            const OneTexel white({ 255, 255, 255, 255 });
            const std::array<TextureData, 1> puff{ white.describe() };

            // One source at a time, so what the lid takes is never ambiguous.
            enum class Source
            {
                Sun,
                Sky,
                Lamp,
            };

            const auto sprited = [&](bool lidded, Source source) {
                SceneDesc scene;
                const Index cut = scene.addTexture(VFS::Path::NormalizedView("sprite.dds"));
                const std::array<Sprite, 1> sprites{ Sprite{
                    .mPosition = osg::Vec3f(0.0f, 0.0f, 0.0f), .mRadius = 60.0f, .mAlpha = 1.0f } };
                scene.addEmitter(sprites, cut, false);

                if (lidded)
                    scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                        .mMesh = scene.addMesh(makeSheet(4000.0f, 600.0f), {}, {}, sQuadIndices) });

                // Over the lid, so the same one that takes the sun and the sky takes this too.
                if (source == Source::Lamp)
                    scene.addLight(Light{ .mPosition = osg::Vec3f(0.0f, 0.0f, 800.0f),
                        .mIntensity = osg::Vec3f(4.0e5f, 4.0e5f, 4.0e5f),
                        .mReach = 4000.0f });

                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -1.0f, 400.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);

                camera.mSkyHorizon = osg::Vec3f();
                camera.mSkyZenith = osg::Vec3f();
                camera.mAmbientFromSky = 1.0f;
                camera.mSunPosition = osg::Vec3f(0.0f, 0.0f, 1.0f);
                camera.mSunIrradiance = source == Source::Sun ? osg::Vec3f(4.0f, 4.0f, 4.0f) : osg::Vec3f();
                camera.mAmbient = source == Source::Sky ? osg::Vec3f(0.5f, 0.5f, 0.5f) : osg::Vec3f();

                std::vector<std::uint8_t> pixels;
                countHits(scene, puff, camera, size, pixels);

                return mRadiance[centre];
            };

            for (const Source source : { Source::Sun, Source::Sky, Source::Lamp })
            {
                const float open = sprited(false, source);
                ASSERT_GT(open, 0.01f) << "the source did not reach the sprite at all";

                EXPECT_LT(sprited(true, source), 0.05f * open) << "a lid did not stop it";
            }
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
        /// puts `d.z` evenly on `[-1, 1]`, so the share left is `h / reach` and nothing else. At 35
        /// and 70 units of a 140-unit reach that is a quarter and a half of the open fill.
        ///
        /// **And it is the sphere the law comes from.** Drawing the cosine about the up instead
        /// would leave `(h / reach)^2` — a sixteenth and a quarter — which the tolerance below is
        /// nowhere near.
        ///
        /// The sprite is opaque and carries no lighting bake, so what a pixel shows is the fill
        /// alone. Only the middle row is read: those rays are level, so they meet neither sheet and
        /// the chord they cut is the same one in all three scenes.
        TEST_F(RtxVisibilityTest, aRoomsFillReachesAPuffFromEverySideAndWhatIsNearTakesItAway)
        {
            constexpr std::uint32_t size = 33;
            constexpr float reach = 140.0f;

            const OneTexel white({ 255, 255, 255, 255 });
            const std::array<TextureData, 1> puff{ white.describe() };

            const auto boxedAt = [&](float half) {
                SceneDesc scene;
                const Index cut = scene.addTexture(VFS::Path::NormalizedView("sprite.dds"));
                const std::array<Sprite, 1> sprites{ Sprite{
                    .mPosition = osg::Vec3f(0.0f, 0.0f, 0.0f), .mRadius = 40.0f, .mAlpha = 1.0f } };
                scene.addEmitter(sprites, cut, false);

                // Nothing at all where the fill is whole, rather than sheets moved out of reach: a
                // scene with no geometry is the one case where the answer cannot be the geometry's.
                if (half > 0.0f)
                    for (const float z : { half, -half })
                        scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                            .mMesh = scene.addMesh(makeSheet(4000.0f, z), {}, {}, sQuadIndices) });

                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -reach, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);

                camera.mSkyHorizon = osg::Vec3f();
                camera.mSkyZenith = osg::Vec3f();
                camera.mSunIrradiance = osg::Vec3f();
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

            EXPECT_NEAR(boxedAt(35.0f) / open, 35.0f / reach, 0.04f) << "a quarter of the sphere is left";
            EXPECT_NEAR(boxedAt(70.0f) / open, 70.0f / reach, 0.04f) << "and half of it at twice the room";
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
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;

            const OneTexel half({ 255, 255, 255, 128 });
            const std::array<TextureData, 1> puff{ half.describe() };

            const auto through = [&](float height, bool sprited) {
                SceneDesc scene;
                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                    .mMesh = scene.addMesh(makeSheet(4000.0f, 0.0f), {}, {}, sQuadIndices) });

                if (sprited)
                {
                    const Index cut = scene.addTexture(VFS::Path::NormalizedView("sprite.dds"));
                    const std::array<Sprite, 1> sprites{ Sprite{
                        .mPosition = osg::Vec3f(0.0f, 0.0f, height), .mRadius = 60.0f, .mAlpha = 1.0f } };
                    scene.addEmitter(sprites, cut, false);
                }

                // Forty up over four hundred along, so the centre ray runs down through the
                // sprite's centre and on to the floor, near or far.
                Shaders::VisibilityConstants camera = makeCamera(osg::Vec3f(0.0f, -400.0f, height + 40.0f),
                    osg::Vec3f(0.0f, 0.0f, height), 60.0f, size, size, 100000.0f);

                // An even sky lights the floor exactly and the puff not at all: a puff is lit by the
                // ambient, the sun and the lamps, and none of those is here.
                camera.mSkyHorizon = osg::Vec3f(0.6f, 0.6f, 0.6f);
                camera.mSkyZenith = camera.mSkyHorizon;
                camera.mAmbientFromSky = 1.0f;
                camera.mAmbient = osg::Vec3f();
                camera.mSunIrradiance = osg::Vec3f();

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
        /// A texel of alpha `128/255 = 0.50196` adds that share of `FLAME_INTENSITY` on its own,

        /// which is what it always added. Two of them screen — `1 - 0.49804^2 = 0.75196` of it —
        /// where a sum would have reached `1.00392`. The original's framebuffer clamped that sum at
        /// one, and this is the smooth form of the same limit.
        TEST_F(RtxVisibilityTest, flamesSaturateWhereTheOriginalClamped)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;

            const OneTexel half({ 255, 255, 255, 128 });
            const std::array<TextureData, 1> flame{ half.describe() };

            const auto glowing = [&](std::size_t count) {
                SceneDesc scene;
                const Index cut = scene.addTexture(VFS::Path::NormalizedView("sprite.dds"));
                const std::vector<Sprite> flames(
                    count, Sprite{ .mPosition = osg::Vec3f(0.0f, 0.0f, 0.0f), .mRadius = 60.0f, .mAlpha = 1.0f });
                scene.addEmitter(flames, cut, true);

                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -400.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
                camera.mSkyHorizon = osg::Vec3f();
                camera.mSkyZenith = osg::Vec3f();
                camera.mAmbient = osg::Vec3f();
                camera.mSunIrradiance = osg::Vec3f();

                std::vector<std::uint8_t> pixels;
                countHits(scene, flame, camera, size, pixels);

                return mRadiance[centre];
            };

            EXPECT_NEAR(glowing(1), Shaders::FLAME_INTENSITY * sHalfAlpha, 0.01f) << "one adds what it painted";
            EXPECT_NEAR(
                glowing(2), Shaders::FLAME_INTENSITY * (1.0f - (1.0f - sHalfAlpha) * (1.0f - sHalfAlpha)), 0.01f)
                << "two screen rather than sum";
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

        TEST_F(RtxVisibilityTest, aPuffIsLitByItsSideAndByWhatItsTextureLetsThrough)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;

            const OneTexel half({ 255, 255, 255, 128 });

            const auto lit = [&](const osg::Vec3f& sun, std::array<std::uint8_t, 4> through) {
                const OneTexel shade(through);
                const std::array<TextureData, 2> textures{ half.describe(), shade.describe() };

                SceneDesc scene;
                const Index cut = scene.addTexture(VFS::Path::NormalizedView("sprite.dds"));
                const Index bake
                    = scene.addBakedTexture(SpriteLightMap::keyFor(VFS::Path::NormalizedView("sprite.dds")));
                const std::array<Sprite, 1> sprites{ Sprite{
                    .mPosition = osg::Vec3f(0.0f, 0.0f, 0.0f), .mRadius = 60.0f, .mAlpha = 1.0f } };
                scene.addEmitter(sprites, cut, false, osg::Vec3f(), osg::Vec3f(), bake);

                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -400.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
                camera.mSkyHorizon = osg::Vec3f();
                camera.mSkyZenith = osg::Vec3f();
                camera.mAmbient = osg::Vec3f();
                camera.mAmbientFromSky = 1.0f;
                camera.mSunPosition = sun;
                camera.mSunIrradiance = osg::Vec3f(4.0f, 4.0f, 4.0f);

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
        /// does not. `SpriteShade` counts one whole layer, and the shader thins the sun by the
        /// texture's mean alpha — its one texel, `128/255` — to `0.49804` of the card's worth from
        /// the side. Nothing stands over either, so the sky is untouched, and the ambient is nought.
        TEST_F(RtxVisibilityTest, aPuffInTheShadeOfItsOwnEmitterIsThinnedByOneLayer)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;

            const OneTexel half({ 255, 255, 255, 128 });
            const std::array<TextureData, 1> puff{ half.describe() };

            const auto lit = [&](bool shaded) {
                SceneDesc scene;
                const Index cut = scene.addTexture(VFS::Path::NormalizedView("sprite.dds"));
                std::vector<Sprite> sprites{ Sprite{
                    .mPosition = osg::Vec3f(0.0f, 0.0f, 0.0f), .mRadius = 60.0f, .mAlpha = 1.0f } };
                if (shaded)
                    sprites.push_back(
                        Sprite{ .mPosition = osg::Vec3f(100.0f, 0.0f, 0.0f), .mRadius = 60.0f, .mAlpha = 1.0f });
                scene.addEmitter(sprites, cut, false);

                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -400.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
                camera.mSkyHorizon = osg::Vec3f();
                camera.mSkyZenith = osg::Vec3f();
                camera.mAmbient = osg::Vec3f();
                camera.mAmbientFromSky = 1.0f;
                camera.mSunPosition = osg::Vec3f(1.0f, 0.0f, 0.0f);
                camera.mSunIrradiance = osg::Vec3f(4.0f, 4.0f, 4.0f);

                std::vector<std::uint8_t> pixels;
                countHits(scene, puff, camera, size, pixels);

                return mRadiance[centre];
            };

            const float alone = lit(false);
            ASSERT_GT(alone, 0.1f) << "the sun did not reach the puff at all";
            EXPECT_NEAR(lit(true) / alone, 1.0f - sHalfAlpha, 0.01f) << "one layer of a half-alpha texture";
        }

        /// What each mask names, and what neither of them does.
        ///
        /// **A mask that says stop accumulating is a mask that says keep the noise**, so what it
        /// names has to be only what no motion vector describes. It once named all water as well, on
        /// the reasoning that a reflection moves with the surface carrying it — which was true, and
        /// stopped being the answer the moment water got a reflection vector of its own. A third of
        /// a Balmora frame was being held at one sample for a question that had been answered.
        ///
        /// What is left is the pixel a sprite reached and did not win: some share of what it shows
        /// went somewhere the vector written for it does not point.
        TEST_F(RtxVisibilityTest, theBiasMaskNamesOnlyWhatNoMotionVectorDescribes)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = std::size_t{ size / 2 } * size + size / 2;

            // The ladder's first level is 40 of 255, so a sprite cut from it covers a sixth of what
            // is behind it — it reaches the pixel and comes nowhere near owning it.
            TestTexture ladder;
            makeMipLadder(ladder);
            const std::span<const TextureData> textures(&ladder.mData, 1);

            SceneDesc scene = makeFlooded(4000.0f, 40.0f);
            const Index cut = scene.addTexture(VFS::Path::NormalizedView("sprite.dds"));
            const std::array<Sprite, 1> sprites{ Sprite{
                .mPosition = osg::Vec3f(-80.0f, 0.0f, 200.0f), .mRadius = 40.0f, .mAlpha = 1.0f } };
            scene.addEmitter(sprites, cut, false);

            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -1.0f, 400.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
            camera.mAmbient = osg::Vec3f(1.0f, 1.0f, 1.0f);
            camera.mWaterLevel = 0.0f;

            std::vector<std::uint8_t> pixels;
            countHits(scene, textures, camera, size, pixels);

            std::vector<float> particles;
            std::vector<float> bias;
            mRenderer->readChannel(Channel::ParticleMask, particles);
            mRenderer->readChannel(Channel::BiasMask, bias);
            ASSERT_EQ(particles.size(), std::size_t{ size } * size);

            const auto sprited = std::find(particles.begin(), particles.end(), 1.0f);
            ASSERT_NE(sprited, particles.end()) << "the emitter reached no pixel at all";
            const std::size_t covered = static_cast<std::size_t>(sprited - particles.begin());

            EXPECT_EQ(bias[covered], 1.0f) << "a sixth of a pixel of sprite wins no vector and is described by none";

            // **The centre is water and nothing else**, which is the whole of the fix: it reflects,
            // it has a vector for what it reflects, and it is not on either mask.
            EXPECT_EQ(particles[centre], 0.0f) << "no sprite over the middle of the frame";
            EXPECT_EQ(bias[centre], 0.0f) << "and water is described rather than given up on";

            // The two are still different populations, and both are a small part of the frame.
            const std::ptrdiff_t marked = std::count(bias.begin(), bias.end(), 1.0f);
            EXPECT_GT(marked, 0);
            EXPECT_LT(marked, static_cast<std::ptrdiff_t>(bias.size()) / 4)
                << "a bias mask over a quarter of a frame is a quarter of a frame held at one sample";
        }

        /// A pixel a sprite mostly is moves the way that sprite did, and not the way the wall
        /// behind it did.
        ///
        /// **The half of the problem the masks only apologise for.** One motion vector is written
        /// per pixel, and it used to be the surface's whatever stood in front of it — so a raindrop
        /// crossing a wall was reprojected as though it were the wall, every frame. The particle
        /// carries its own travel now, off `osgParticle`'s own previous position.
        TEST_F(RtxVisibilityTest, aPixelASpriteOwnsCarriesTheSpritesOwnMotion)
        {
            constexpr std::uint32_t size = 33;

            // Two hundred units under the eye, so a unit across is `size / (2 * 200 * tan(30 deg))`
            // of a pixel: 33 / 230.94 = 0.14289. A sprite that travelled sixty units across is
            // 8.573 pixels of screen motion, and nothing else in the frame moves at all.
            constexpr float travel = 60.0f;
            constexpr float expected = 33.0f * travel / (2.0f * 200.0f * 0.5773503f);

            TestTexture sheet;
            makeOpaqueSheet(sheet);
            const std::span<const TextureData> textures(&sheet.mData, 1);

            SceneDesc scene = makeFlooded(4000.0f, 40.0f);
            const Index cut = scene.addTexture(VFS::Path::NormalizedView("sprite.dds"));
            const std::array<Sprite, 1> sprites{ Sprite{ .mPosition = osg::Vec3f(0.0f, 0.0f, 200.0f),
                .mRadius = 40.0f,
                .mAlpha = 1.0f,
                .mMoved = osg::Vec3f(travel, 0.0f, 0.0f) } };
            scene.addEmitter(sprites, cut, false);

            const Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -1.0f, 400.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);

            // **Twice, with the camera held still.** The first frame has no past to reproject
            // against, so what the second one writes is the sprite's travel and nothing else.
            std::vector<std::uint8_t> pixels;
            countHits(scene, textures, camera, size, pixels);
            mRenderer->renderFrame(camera, FrameOptions{});

            std::vector<float> covered;
            std::vector<float> moved;
            mRenderer->readChannel(Channel::ParticleMask, covered);
            mRenderer->readChannel(Channel::Motion, moved);

            const auto sprited = std::find(covered.begin(), covered.end(), 1.0f);
            ASSERT_NE(sprited, covered.end()) << "the emitter reached no pixel at all";
            const std::size_t at = static_cast<std::size_t>(sprited - covered.begin());

            // A corner, which the sprite is nowhere near: still water under a still camera.
            EXPECT_NEAR(moved[0], 0.0f, 0.01f) << "nothing else in the frame moved";
            EXPECT_NEAR(moved[1], 0.0f, 0.01f);

            EXPECT_NEAR(std::abs(moved[at * 2]), expected, 0.5f)
                << "the sprite's own travel, projected at the depth it hangs at";
            EXPECT_NEAR(moved[at * 2 + 1], 0.0f, 0.5f) << "and it travelled across rather than along";

            // **The parameter has to matter**, or this measures a coincidence: the same frame with a
            // particle that did not move writes the surface's nought instead.
            SceneDesc still = makeFlooded(4000.0f, 40.0f);
            const Index cutAgain = still.addTexture(VFS::Path::NormalizedView("sprite.dds"));
            const std::array<Sprite, 1> stopped{ Sprite{
                .mPosition = osg::Vec3f(0.0f, 0.0f, 200.0f), .mRadius = 40.0f, .mAlpha = 1.0f } };
            still.addEmitter(stopped, cutAgain, false);

            countHits(still, textures, camera, size, pixels);
            mRenderer->renderFrame(camera, FrameOptions{});
            mRenderer->readChannel(Channel::Motion, moved);

            EXPECT_NEAR(moved[at * 2], 0.0f, 0.01f) << "a particle that stood still moved nothing";

            // **And the kind that hides nothing.** A flame blends additively, so it leaves the
            // transmittance at one however bright it is: no measure of coverage will ever find it,
            // and the rule that only asked about coverage left its glow reprojected as the water
            // under it. It owns the pixel by outshining what the layer left instead.
            SceneDesc flame = makeFlooded(4000.0f, 40.0f);
            const Index cutFlame = flame.addTexture(VFS::Path::NormalizedView("sprite.dds"));
            const std::array<Sprite, 1> burning{ Sprite{ .mPosition = osg::Vec3f(0.0f, 0.0f, 200.0f),
                .mRadius = 40.0f,
                .mAlpha = 1.0f,
                .mMoved = osg::Vec3f(travel, 0.0f, 0.0f) } };
            flame.addEmitter(burning, cutFlame, true);

            countHits(flame, textures, camera, size, pixels);
            mRenderer->renderFrame(camera, FrameOptions{});

            std::vector<float> lit;
            mRenderer->readChannel(Channel::ParticleMask, lit);
            mRenderer->readChannel(Channel::Motion, moved);

            const auto glowing = std::find(lit.begin(), lit.end(), 1.0f);
            ASSERT_NE(glowing, lit.end()) << "an additive sprite is still a sprite the mask names";
            const std::size_t over = static_cast<std::size_t>(glowing - lit.begin());

            EXPECT_NEAR(std::abs(moved[over * 2]), expected, 0.5f)
                << "the flame's own travel, though it covered nothing at all";
        }
    }
}
