#include "fixture.hpp"

namespace Rtx::Testing
{
    namespace
    {
        /// A camera with nothing in the air but the world's edge, under an even sky.
        ///
        /// The weather's own extinction stays at nothing, so the second element of the air is the
        /// only thing between the eye and what it looks at. An even sky, because a wall's radiance
        /// is then exactly its albedo times that one number whatever direction the bounce takes.
        Shaders::VisibilityConstants underTheEdge(const osg::Vec3f& eye, std::uint32_t size, float edge)
        {
            Shaders::VisibilityConstants camera
                = makeCamera(eye, osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 200000.0f);

            camera.mSkyHorizon = osg::Vec3f(sFoggySky, sFoggySky, sFoggySky);
            camera.mSkyZenith = camera.mSkyHorizon;
            camera.mAmbientFromSky = 1.0f;
            camera.mFogEdge = edge;
            return camera;
        }

        /// A ray that hits nothing comes back with the sky the weather named, not a constant.
        TEST_F(RtxVisibilityTest, theSkyIsTheWeathersOwnColourAndRunsFromHorizonToZenith)
        {
            constexpr std::uint32_t size = 33;

            // Facing straight up, so the centre pixel looks at the zenith and the frame's edge looks
            // sixty degrees off it. Nothing is placed, so every ray misses.
            Shaders::VisibilityConstants camera
                = makeCamera(osg::Vec3f(0.0f, 0.0f, 0.0f), osg::Vec3f(0.0f, 1.0f, 0.0f), 60.0f, size, size, 10000.0f);
            camera.mSkyHorizon = osg::Vec3f(1.0f, 0.0f, 0.0f);
            camera.mSkyZenith = osg::Vec3f(0.0f, 0.0f, 1.0f);
            camera.mAmbientFromSky = 1.0f;

            SceneDesc scene = makeWall();
            std::vector<std::uint8_t> pixels;
            countHits(scene, {}, camera, size, pixels);

            // The camera looks level, so the middle row's rays are horizontal: z of zero, which is
            // the horizon end of the mix exactly. Pure red, and no blue at all.
            const std::size_t middle = (std::size_t{ size / 2 } * size + size / 2) * 4;
            EXPECT_EQ(pixels[middle], 255) << "the horizon colour, undiluted";
            EXPECT_EQ(pixels[middle + 2], 0);

            // The top row tilts up by tan(30) of the half-frame, so its z is sin of that angle and
            // the mix has moved toward the zenith. Only the direction of the move is asserted: the
            // exact angle is the camera's business and has its own test.
            const std::size_t top = std::size_t{ size / 2 } * 4;
            EXPECT_LT(pixels[top], 255) << "less horizon overhead";
            EXPECT_GT(pixels[top + 2], 0) << "and some zenith";
        }

        /// Both moons light a floor, and the two slots are one code path.
        ///
        /// **What the pair of them costs is one loop, so what proves the loop is the second slot.**
        /// Masser and Secunda are carried in an array of two and gathered by one pass over it; a
        /// shader that reached only the first entry would leave every Secunda-lit night dark, and
        /// nothing in a picture would say so while the brighter moon was up. So the same moon is put
        /// in each slot in turn and has to light the same floor by the same amount.
        ///
        /// A floor of albedo 0.5 under an irradiance of 2 straight down returns `0.5 * 2 / pi`,
        /// which is 0.31831. Nothing stands on the floor, so the shadow ray always clears and the
        /// estimate carries no variance to average away.
        TEST_F(RtxVisibilityTest, bothMoonsLightAndTheTwoSlotsAreOneCodePath)
        {
            constexpr std::uint32_t size = 32;

            SceneDesc scene;
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                .mMesh = scene.addMesh(makeSheet(4000.0f, 0.0f), {}, {}, sQuadIndices) });

            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -1.0f, 300.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);

            // Nothing else lights the floor, so what arrives is the moon's alone.
            camera.mSkyHorizon = osg::Vec3f();
            camera.mSkyZenith = osg::Vec3f();
            camera.mSunIrradiance = osg::Vec3f();

            Shaders::MoonDisc overhead{};
            overhead.mDirection = osg::Vec3f(0.0f, 0.0f, 1.0f);
            overhead.mRight = osg::Vec3f(1.0f, 0.0f, 0.0f);
            overhead.mUp = osg::Vec3f(0.0f, 1.0f, 0.0f);
            overhead.mColour = osg::Vec3f(1.0f, 1.0f, 1.0f);
            overhead.mIrradiance = osg::Vec3f(2.0f, 2.0f, 2.0f);
            overhead.mAngularRadius = moonAngularRadius(Moon::Masser);
            overhead.mAlpha = 1.0f;
            overhead.mFace = Shaders::NO_TEXTURE;

            const auto litFromSlot = [&](std::size_t slot) {
                camera.mMoons[0] = Shaders::MoonDisc{};
                camera.mMoons[1] = Shaders::MoonDisc{};
                camera.mMoons[slot] = overhead;

                std::vector<std::uint8_t> pixels;
                EXPECT_EQ(countHits(scene, {}, camera, size, pixels), size * size);

                return decodeSrgb(pixels[(std::size_t{ size / 2 } * size + size / 2) * 4]);
            };

            EXPECT_NEAR(litFromSlot(0), 0.31831f, 0.005f);
            EXPECT_NEAR(litFromSlot(1), 0.31831f, 0.005f) << "the second moon lights nothing";

            // And a moon that delivers nothing lights nothing, which is what a daylit frame reaches
            // for both of them and what keeps it from tracing two shadow rays for no light.
            camera.mMoons[0] = Shaders::MoonDisc{};
            camera.mMoons[1] = Shaders::MoonDisc{};

            std::vector<std::uint8_t> dark;
            EXPECT_EQ(countHits(scene, {}, camera, size, dark), size * size);
            EXPECT_FLOAT_EQ(decodeSrgb(dark[(std::size_t{ size / 2 } * size + size / 2) * 4]), 0.0f);
        }

        /// A moon hides the sky behind it, which is the order the engine draws its own in.
        ///
        /// **`SkyManager::create` builds the sky as atmosphere, night sky, sun, Masser, Secunda,
        /// cloud**, and `paintMoon` writes `color.a = maskAlpha` under a `(ONE, ONE_MINUS_SRC_ALPHA)`
        /// blend — so an opaque moon replaces whatever the sun and the other moon put behind it.
        /// This renderer added the moons to the sky instead and took a share of the sun alone.
        ///
        /// **The star field is not in this any more and cannot be.** It is drawn by the display
        /// pass, at the resolution the frame is shown at, so nothing of it reaches the channel this
        /// measures — `ToneConstants::mStars` carries why.
        ///
        /// A full moon of white at the middle of its own disc is `MOON_RADIANCE`: the incidence and
        /// the emission cosines are both one there, so McEwen's term is `2 * 1 / (1 + 1)`, and the
        /// sky behind it is set to nothing so no gradient is in the way.
        TEST_F(RtxVisibilityTest, aMoonHidesWhatStandsBehindIt)
        {
            constexpr std::uint32_t size = 32;
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;

            SceneDesc scene;
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                .mMesh = scene.addMesh(makeSheet(4000.0f, -2000.0f), {}, {}, sQuadIndices) });

            // Forty-five degrees up along `+y`, which keeps the camera off its own pole.
            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, 0.0f, 0.0f), osg::Vec3f(0.0f, 1000.0f, 1000.0f), 60.0f, size, size, 100000.0f);

            camera.mSkyHorizon = osg::Vec3f();
            camera.mSkyZenith = osg::Vec3f();
            camera.mSunIrradiance = osg::Vec3f();

            const float root = std::sqrt(0.5f);
            Shaders::MoonDisc facing{};
            facing.mDirection = osg::Vec3f(0.0f, root, root);
            facing.mRight = osg::Vec3f(1.0f, 0.0f, 0.0f);
            facing.mUp = osg::Vec3f(0.0f, -root, root);
            facing.mColour = osg::Vec3f(1.0f, 1.0f, 1.0f);
            facing.mAngularRadius = 0.2f;
            facing.mAlpha = 1.0f;
            facing.mThroughAir = osg::Vec3f(1.0f, 1.0f, 1.0f);
            facing.mFace = Shaders::NO_TEXTURE;

            const auto sky = [&](std::size_t at) {
                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels);

                return mRadiance[at];
            };

            camera.mMoons[0] = facing;
            EXPECT_NEAR(sky(centre), Shaders::MOON_RADIANCE, 0.01f) << "the disc is not what it should be";

            // The sun put exactly behind it, which is what an eclipse is and what the moons used to
            // take their share of alone.
            camera.mSunPosition = facing.mDirection;
            camera.mSunIrradiance = osg::Vec3f(8.0f, 8.0f, 8.0f);
            camera.mSunDiscColour = osg::Vec3f(1.0f, 1.0f, 1.0f);
            EXPECT_NEAR(sky(centre), Shaders::MOON_RADIANCE, 0.01f) << "the sun came through the moon";

            // And the second moon takes its share of the first, which no maximum over the two could
            // say: put in front, it replaces Masser rather than being added to it.
            camera.mMoons[1] = facing;
            camera.mMoons[1].mColour = osg::Vec3f(0.25f, 0.25f, 0.25f);
            EXPECT_NEAR(sky(centre), 0.25f * Shaders::MOON_RADIANCE, 0.01f) << "two moons were added together";
        }

        /// The deck shadows the ground under it, and what darkens is the alpha over the sheet's mean.
        ///
        /// **The one occluder no ray finds.** The clouds are not in the acceleration structure, so
        /// `cloudShadow` asks the sheet directly where the ray from a shading point to a light
        /// crosses the layer. `CLOUD_SHADOW_DEPTH` says why it is the alpha *over the sheet's own
        /// mean* that darkens: the content has already dimmed the sun for the weather, and taking
        /// the whole of the alpha would state that twice.
        ///
        /// A floor of albedo 0.5 under a sun of 2 delivers `0.5 * 2 / pi` where nothing stands over
        /// it. A sheet whose alpha is one everywhere then darkens it by `exp(-4)` where the sheet's
        /// own mean is nought, and by nothing at all where that mean is one — which is the overcast
        /// case, and the whole point of measuring against the mean.
        TEST_F(RtxVisibilityTest, theDeckShadowsWhatStandsUnderIt)
        {
            constexpr std::uint32_t size = 32;
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;

            SceneDesc scene;
            scene.addTexture(VFS::Path::NormalizedView("cloud.dds"));
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                .mMesh = scene.addMesh(makeSheet(4000.0f, 0.0f), {}, {}, sQuadIndices) });

            constexpr std::array<std::uint8_t, 4> solid{ 255, 255, 255, 255 };
            const MipLevel one{ 0, 1, 1 };
            const std::array<TextureData, 1> sheet{ TextureData{
                .mFormat = TextureFormat::Rgba8Unorm,
                .mWidth = 1,
                .mHeight = 1,
                .mBytes = std::as_bytes(std::span(solid)),
                .mLevels = std::span(&one, 1),
            } };

            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -1.0f, 300.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);

            // Nothing else lights the floor, so what arrives is the sun's alone.
            camera.mSkyHorizon = osg::Vec3f();
            camera.mSkyZenith = osg::Vec3f();
            camera.mSunPosition = osg::Vec3f(0.0f, 0.0f, 1.0f);
            camera.mSunIrradiance = osg::Vec3f(2.0f, 2.0f, 2.0f);

            camera.mClouds = Shaders::CloudDeck{
                .mOpacity = 1.0f,
                .mAltitude = 30000.0f,
                .mPerTile = osg::Vec2f(0.001f, 0.001f),
                .mTexture = 0u,
                .mNext = Shaders::NO_TEXTURE,
            };

            const auto floorUnder = [&](float cover) {
                camera.mClouds.mCover = cover;

                std::vector<std::uint8_t> pixels;
                countHits(scene, sheet, camera, size, pixels);

                return mRadiance[centre];
            };

            EXPECT_NEAR(floorUnder(1.0f), 0.31831f, 1.0e-4f) << "a sheet at its own mean darkens nothing";
            EXPECT_NEAR(floorUnder(0.0f), 0.31831f * std::exp(-4.0f), 1.0e-4f) << "and one over it darkens by four";

            // And nothing at all where there is no deck, whatever the sheet says — which is the test
            // every frame with no cloud over it passes without knowing it.
            camera.mClouds.mOpacity = 0.0f;
            EXPECT_NEAR(floorUnder(0.0f), 0.31831f, 1.0e-4f);
        }

        /// The deck takes its shape from what the sheet paints, read against what that sheet averages.
        ///
        /// **A sheet of one white texel with the mean moved under it**, which is `CloudDeck::mMean`'s
        /// ratio measured four times with no filter in the way. The ratio reaches a cloud in full sun
        /// at `CLOUD_THICKNESS_MAX` times the mean, so a white texel against a mean of one is half
        /// way between the two colours the deck was handed, against a half it is fully lit, and
        /// against a quarter it is over and held there. A sheet nothing could average takes no ratio
        /// and reads as the average cloud it could not measure.
        ///
        /// The sky behind it is set to nothing and the deck covers everything, so what the middle
        /// pixel carries is the deck alone.
        TEST_F(RtxVisibilityTest, theDeckTakesItsShapeFromWhatTheSheetPaints)
        {
            constexpr std::uint32_t size = 32;
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;

            SceneDesc scene;
            scene.addTexture(VFS::Path::NormalizedView("cloud.dds"));
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                .mMesh = scene.addMesh(makeSheet(4000.0f, -2000.0f), {}, {}, sQuadIndices) });

            constexpr std::array<std::uint8_t, 4> white{ 255, 255, 255, 255 };
            const MipLevel one{ 0, 1, 1 };
            const std::array<TextureData, 1> sheet{ TextureData{
                .mFormat = TextureFormat::Rgba8Unorm,
                .mWidth = 1,
                .mHeight = 1,
                .mBytes = std::as_bytes(std::span(white)),
                .mLevels = std::span(&one, 1),
            } };

            // Forty-five degrees up, which puts every ray on the deck and none of them on its pole.
            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, 0.0f, 0.0f), osg::Vec3f(0.0f, 1000.0f, 1000.0f), 60.0f, size, size, 100000.0f);

            camera.mSkyHorizon = osg::Vec3f();
            camera.mSkyZenith = osg::Vec3f();
            camera.mSunIrradiance = osg::Vec3f();

            // A flat layer whose fade is far outside the piece of it this sees, so the deck is whole
            // across the frame and the only thing moving is the sheet against its mean.
            camera.mClouds = Shaders::CloudDeck{
                .mOpacity = 1.0f,
                .mLit = osg::Vec3f(0.6f, 0.6f, 0.6f),
                .mShadowed = osg::Vec3f(0.2f, 0.2f, 0.2f),
                .mMean = 1.0f,
                .mAltitude = 1000.0f,
                .mPerTile = osg::Vec2f(0.01f, 0.01f),
                .mRings = osg::Vec3f(100.0f, 200.0f, 300.0f),
                .mTexture = 0u,
                .mNext = Shaders::NO_TEXTURE,
            };

            const auto deck = [&](float mean) {
                camera.mClouds.mMean = mean;

                std::vector<std::uint8_t> pixels;
                countHits(scene, sheet, camera, size, pixels);

                return mRadiance[centre];
            };

            EXPECT_NEAR(deck(1.0f), 0.4f, 1.0e-3f) << "a texel at its sheet's own mean is half lit";
            EXPECT_NEAR(deck(0.5f), 0.6f, 1.0e-3f) << "twice the mean is a cloud in full sun";
            EXPECT_NEAR(deck(0.25f), 0.6f, 1.0e-3f) << "and four times over is held there";
            EXPECT_NEAR(deck(0.0f), 0.4f, 1.0e-3f) << "a sheet nobody could average took a ratio anyway";
        }

        /// The display pass draws the star field, and draws it only where a ray reached the sky.
        ///
        /// **It is drawn there because a point source is what an upscaler removes.** The trace no
        /// longer draws the field at all, so the only place it can be checked is the picture — after
        /// the tone curve, which is what `renderPicture` gives. What that costs is exactness: the
        /// assertions below are about which pixels carry a star, not about how bright one is.
        ///
        /// **A sheet of one white texel**, so every direction the field reaches carries one. Half the
        /// view is a floor four hundred units down and half is sky, and the floor must be as dark
        /// with the field as without it — a star drawn over geometry is the failure this guards.
        TEST_F(RtxVisibilityTest, theDisplayPassDrawsTheFieldOnlyOverSky)
        {
            constexpr std::uint32_t size = 32;

            SceneDesc scene;
            scene.addTexture(VFS::Path::NormalizedView("white.dds"));
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                .mMesh = scene.addMesh(makeSheet(4000.0f, -400.0f), {}, {}, sQuadIndices) });

            constexpr std::array<std::uint8_t, 4> white{ 255, 255, 255, 255 };
            const MipLevel one{ 0, 1, 1 };
            const std::array<TextureData, 1> sheet{ TextureData{
                .mFormat = TextureFormat::Rgba8Unorm,
                .mWidth = 1,
                .mHeight = 1,
                .mBytes = std::as_bytes(std::span(white)),
                .mLevels = std::span(&one, 1),
            } };

            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -2000.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);

            camera.mSkyHorizon = osg::Vec3f();
            camera.mSkyZenith = osg::Vec3f();
            camera.mSunIrradiance = osg::Vec3f();

            const auto brightest = [&](bool stars) {
                camera.mStars = stars ? Shaders::StarField{ .mFade = 1.0f,
                    .mTurn = 0.0f,
                    .mTile = 1.0f,
                    .mHorizon = 0.0f,
                    .mTexture = 0u }
                                      : noStars();

                std::vector<std::uint8_t> pixels;
                renderPicture(scene, camera, size, pixels, sheet);

                std::array<std::uint8_t, 2> halves{ 0, 0 };
                for (std::uint32_t row = 0; row < size; ++row)
                    for (std::uint32_t column = 0; column < size; ++column)
                    {
                        const std::size_t at = (std::size_t{ row } * size + column) * 4;
                        const std::size_t half = row < size / 2 ? 0u : 1u;

                        halves[half] = std::max(halves[half], pixels[at]);
                    }

                return halves;
            };

            const std::array<std::uint8_t, 2> without = brightest(false);
            const std::array<std::uint8_t, 2> with = brightest(true);

            EXPECT_EQ(without[0], 0) << "the sky was not empty to begin with";
            EXPECT_GT(with[0], 128) << "the field was not drawn at all";
            EXPECT_EQ(with[1], without[1]) << "a star was drawn over the floor";

            // **And a moon puts the field out, which is the one thing this pass cannot see.** The
            // sky is composited in `skyRadiance` and the field is drawn after the upscaler, so what
            // the moons and the deck left of it has to travel with the pixel — `GBuffer::getStarsShown`
            // is that number. A disc of black, so what is measured is the covering and not the face.
            const float root = std::sqrt(0.5f);
            Shaders::MoonDisc covering{};
            covering.mDirection = osg::Vec3f(0.0f, root, root);
            covering.mRight = osg::Vec3f(1.0f, 0.0f, 0.0f);
            covering.mUp = osg::Vec3f(0.0f, -root, root);
            covering.mColour = osg::Vec3f();
            covering.mThroughAir = osg::Vec3f(1.0f, 1.0f, 1.0f);
            covering.mAngularRadius = 1.2f;
            covering.mAlpha = 1.0f;
            covering.mFace = Shaders::NO_TEXTURE;

            camera.mMoons[0] = covering;
            EXPECT_EQ(brightest(true)[0], without[0]) << "a star was drawn through a moon";

            // The same disc, not there: it covers nothing and the field comes back. Which is what
            // says the covering was the cause, rather than the black the disc is painted.
            camera.mMoons[0].mAlpha = 0.0f;
            EXPECT_GT(brightest(true)[0], 128) << "the moon was not what put the field out";
        }

        /// The world's edge is nothing over the ground the player stands on and total at the last
        /// cell.
        ///
        /// **The whole reason for a second element of the air.** Morrowind's own fog depth is
        /// measured over the same reach the ground is built to, and clear weather leaves a third of
        /// the last cell showing — so the ring where the terrain stops is visible as a cut. Thicken
        /// the weather until it is not, and every weather becomes a fog bank.
        ///
        /// **An exponential in the range from the eye is what separates the two.** The wall here is
        /// lit by the sky alone, so it reads `0.5 * 0.6 = 0.3` with nothing over it and
        /// `0.6 - 0.3 * T` with the edge in front of it — the sky's own colour in place of what the
        /// edge took. `T` is `(1/256)^crossed` for
        ///
        ///     crossed = (exp(range / 0.125) - 1) / (exp(8) - 1),  range = distance / reach,
        ///
        /// where `distance` is the ray's own and not its shadow on the ground — the camera here runs
        /// level, so the two agree and the figures are about the ramp alone.
        ///
        /// so a half, three quarters and all of the reach come to 0.017986, 0.135045 and 1 of the
        /// ramp — transmittances of 0.90507, 0.47290 and 0.003906, and radiances of 0.32848,
        /// 0.45813 and 0.59883.
        ///
        /// **Those three are the shape and not just the endpoint.** Half the world costs six bytes,
        /// the next quarter costs twenty-five, and the last quarter costs the rest: a uniform medium
        /// reaching the same place at the edge would have taken half of the near wall with it.
        TEST_F(RtxVisibilityTest, theWorldsEdgeClosesOverTheLastCellAndLeavesTheGroundNearby)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;
            constexpr float reach = 32768.0f;

            const auto look = [&](float distance, bool edged) {
                const Shaders::VisibilityConstants camera
                    = underTheEdge(osg::Vec3f(0.0f, -distance, 0.0f), size, edged ? reach : 0.0f);

                std::vector<std::uint8_t> pixels;
                countHits(makeWall(400.0f), {}, camera, size, pixels);
                return int{ pixels[centre] };
            };

            EXPECT_NEAR(look(0.5f * reach, true), int{ encodeSrgb(0.32848f) }, 1) << "half of the world";
            EXPECT_NEAR(look(0.75f * reach, true), int{ encodeSrgb(0.45813f) }, 1) << "three quarters of it";
            EXPECT_NEAR(look(reach, true), int{ encodeSrgb(0.59883f) }, 1) << "and the last cell of it";

            // **Which is the sky's own colour to the byte**, and that is what hides a cut edge: the
            // wall is not merely dim at the reach, it is the thing behind it.
            EXPECT_EQ(look(reach, true), int{ encodeSrgb(sFoggySky) }) << "the last cell, still showing";

            // And with no edge declared the same wall is untouched at every one of those ranges. A
            // room has no ring of cut ground and pays nothing for one.
            for (const float distance : { 0.5f * reach, 0.75f * reach, reach })
                EXPECT_EQ(look(distance, false), int{ encodeSrgb(0.3f) }) << "with no edge at " << distance;
        }

        /// A ray that climbs leaves the world's edge behind, and one that descends never does.
        ///
        /// **What is missing is a ring on the ground and not a dome over it.** Air that closed over
        /// everything at the reach would put the horizon's colour across the whole upper sky, so
        /// `FOG_EDGE_RISE` cuts it off at twenty-five degrees of climb.
        ///
        /// **And at no descent whatever, which is the half that is easy to get wrong.** An eye high
        /// enough to see the ring looks *down* at it — the steeper the view, the more of the cut it
        /// can see — so a mask that read the elevation either way would take the air off exactly
        /// where the world stops hiding itself.
        ///
        /// Three frames of the same wall at the same range, differing in the elevation the eye
        /// reaches it at and in nothing else. Thirty degrees, which is `direction.z` of exactly a
        /// half against the sine of the twenty-five the mask ends at, so the smoothstep is saturated
        /// either way and the answers are the two ends rather than points along it.
        TEST_F(RtxVisibilityTest, aClimbLeavesTheWorldsEdgeBehindAndADescentNeverDoes)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;
            constexpr float reach = 32768.0f;

            // Past the reach along the ray in all three frames, so the ramp is fully crossed and the
            // mask is the only thing left to differ.
            constexpr float slant = reach / 0.8660254f;
            constexpr float climb = reach * 0.5773503f;

            const auto look = [&](const osg::Vec3f& eye) {
                const Shaders::VisibilityConstants camera = underTheEdge(eye, size, reach);

                std::vector<std::uint8_t> pixels;
                countHits(makeWall(400.0f), {}, camera, size, pixels);
                return int{ pixels[centre] };
            };

            EXPECT_EQ(look(osg::Vec3f(0.0f, -reach, -climb)), int{ encodeSrgb(0.3f) })
                << "a wall the eye had to look up at, hidden anyway";
            EXPECT_EQ(look(osg::Vec3f(0.0f, -reach, climb)), int{ encodeSrgb(sFoggySky) })
                << "and the same wall from above it, showing through the cut";
            EXPECT_EQ(look(osg::Vec3f(0.0f, -slant, 0.0f)), int{ encodeSrgb(sFoggySky) })
                << "and the same wall along the ground, still showing";
        }

        /// The world's edge leaves the sky exactly where it was.
        ///
        /// **Because it scatters the sky's own gradient rather than the fog's colour.** The two are
        /// one colour at the horizon — Morrowind records `mFogColour` and `mSkyHorizon` from the
        /// same byte triple — but above it they are not, and air that put the horizon across the
        /// lower sky would flatten the gradient the game draws. Handed the gradient instead, a ray
        /// that reaches nothing gets `g * T + g * (1 - T)`, which is `g`.
        ///
        /// **The fog's colour is set here and is not the sky's**, which is what makes this an
        /// assertion rather than a tautology: an edge that in-scattered `mFogColour` would paint the
        /// lower half of this frame red. Its extinction stays at nothing, so the weather's own march
        /// contributes none of it.
        TEST_F(RtxVisibilityTest, theWorldsEdgeLeavesTheSkyExactlyWhereItWas)
        {
            constexpr std::uint32_t size = 48;

            const auto sky = [&](float edge, std::vector<float>& values) {
                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -50000.0f, 0.0f), osg::Vec3f(0.0f, -60000.0f, 0.0f), 90.0f, size, size, 200000.0f);

                camera.mSkyHorizon = osg::Vec3f(0.10f, 0.20f, 0.40f);
                camera.mSkyZenith = osg::Vec3f(0.40f, 0.50f, 0.90f);
                camera.mAmbientFromSky = 1.0f;
                camera.mFogColour = osg::Vec3f(1.0f, 0.0f, 0.0f);
                camera.mFogEdge = edge;

                // A wall behind the camera, because a scene has to hold something. Every ray in the
                // frame misses it and comes back with the sky alone.
                renderRadiance(makeWall(), camera, size, values);
            };

            std::vector<float> open;
            std::vector<float> edged;
            sky(0.0f, open);
            sky(32768.0f, edged);

            ASSERT_EQ(open.size(), edged.size());
            for (std::size_t at = 0; at < open.size(); ++at)
                ASSERT_NEAR(edged[at], open[at], 1.0e-5f) << "at " << at;

            // **And there was a gradient to leave alone.** Ninety degrees of frame reaches forty-five
            // either side of the horizon, so the top row is most of the way to the zenith and the
            // bottom row is under it — a flat sky would pass the loop above without saying anything.
            const std::size_t bottom = (std::size_t{ size - 1 } * size + size / 2) * 4;
            EXPECT_GT(edged[size / 2 * 4] - edged[bottom], 0.15f) << "the sky's own gradient, still in it";
        }
    }
}
