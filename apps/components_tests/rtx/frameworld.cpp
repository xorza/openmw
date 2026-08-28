#include <array>
#include <cmath>
#include <limits>

#include <gtest/gtest.h>

#include <components/rtx/cloudshell.hpp>
#include <components/rtx/frameworld.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/shaders/visibility.h>
#include <components/rtx/skybuilder.hpp>

namespace Rtx
{
    namespace
    {
        /// A layer to hang a deck on, standing in for what the cloud mesh is read for. Morrowind's
        /// own comes to 0.711 tiles up at a curvature of 0.057; the numbers here are only distinct.
        const Rtx::CloudShell sShell{
            .mTiles = osg::Vec2f(0.75f, -0.75f), .mCurvature = 0.06f, .mRings = osg::Vec3f(1.0f, 1.5f, 2.0f)
        };

        /// What a deck radiates, for the tests that are about everything else it carries. Two
        /// distinct colours, so a field written from the wrong one shows.
        const Rtx::DeckLight sLight{ .mLit = osg::Vec3f(0.5f, 0.6f, 0.7f), .mShadowed = osg::Vec3f(0.1f, 0.2f, 0.3f) };

        /// A world where no two numbers are the same, so a field written from the wrong one shows.
        FrameWorld distinct()
        {
            FrameWorld world{
                .mSun = { .mPosition = osg::Vec3f(0.0f, -0.6f, 0.8f),
                    .mIrradiance = osg::Vec3f(1.5f, 1.25f, 1.0f),
                    .mDiscColour = osg::Vec3f(1.0f, 0.8f, 0.65f) },
                .mAmbient = osg::Vec3f(0.11f, 0.12f, 0.13f),
                .mSkyHorizon = osg::Vec3f(0.21f, 0.22f, 0.23f),
                .mSkyZenith = osg::Vec3f(0.31f, 0.32f, 0.33f),
                .mAir = { .mColour = osg::Vec3f(0.41f, 0.42f, 0.43f),
                    .mExtinction = 1.5e-4f,
                    .mUniform = 0.75f,
                    .mEdge = 24576.0f },
                .mWaterLevel = -37.5f,
                .mSeconds = 12.25f,
            };

            world.mClouds = Rtx::Shaders::CloudDeck{
                .mOpacity = 0.875f,
                .mLit = osg::Vec3f(0.51f, 0.52f, 0.53f),
                .mShadowed = osg::Vec3f(0.11f, 0.12f, 0.13f),
                .mCover = 0.4375f,
                .mAltitude = 34995.6f,
                .mPerTile = osg::Vec2f(0.625f, -0.6875f),
                .mBlend = 0.25f,
                .mScroll = 3.5f,
                .mBearing = osg::Vec2f(0.8f, 0.6f),
                .mNextBearing = osg::Vec2f(0.28f, 0.96f),
                .mCurvature = 0.09375f,
                .mRings = osg::Vec3f(0.8125f, 1.3125f, 1.9375f),
                .mTexture = 4u,
                .mNext = 9u,
            };
            world.mStars = Rtx::Shaders::StarField{ .mFade = 0.75f, .mTurn = 2.5f, .mTexture = 11u };

            world.mMoons[0] = MoonPlacement{
                .mDirection = osg::Vec3f(0.0f, 0.0f, 1.0f),
                .mRight = osg::Vec3f(1.0f, 0.0f, 0.0f),
                .mUp = osg::Vec3f(0.0f, 1.0f, 0.0f),
                .mAngularRadius = 0.1676f,
                .mPhaseAngle = 0.25f,
                .mAlpha = 0.5f,
                .mFace = 7,
                .mColour = osg::Vec3f(0.0332f, 0.0099f, 0.0123f),
            };
            world.mMoons[1] = MoonPlacement{
                .mDirection = osg::Vec3f(0.0f, 1.0f, 0.0f),
                .mRight = osg::Vec3f(-1.0f, 0.0f, 0.0f),
                .mUp = osg::Vec3f(0.0f, 0.0f, 1.0f),
                .mAngularRadius = 0.0719f,
                .mPhaseAngle = 2.5f,
                .mAlpha = 0.25f,
                .mFace = 9,
                .mColour = osg::Vec3f(0.0440f, 0.0373f, 0.0295f),
            };

            return world;
        }

        /// Every number the world decides reaches the constants, and reaches the right one.
        ///
        /// **The test the two paths never had.** The game and the harness each used to write these
        /// twenty-odd fields themselves, which is how the sea's clock came to be filled by one and
        /// left at zero by the other, and how the game's interiors came to run the outdoor fog field.
        /// One conversion is what fixed that; this is what says the conversion is complete.
        ///
        /// Written against a zeroed frame so that a field `applyWorld` forgets stays zero and fails
        /// here, rather than quietly carrying whatever the camera left behind.
        TEST(RtxFrameWorldTest, everyNumberTheWorldDecidesReachesTheFrame)
        {
            const FrameWorld world = distinct();

            Rtx::Shaders::VisibilityConstants constants{};
            applyWorld(world, constants);

            EXPECT_EQ(constants.mSunPosition, world.mSun.mPosition);
            EXPECT_EQ(constants.mSunIrradiance, world.mSun.mIrradiance);
            EXPECT_EQ(constants.mSunDiscColour, world.mSun.mDiscColour);
            EXPECT_EQ(constants.mAmbient, world.mAmbient);
            EXPECT_EQ(constants.mSkyHorizon, world.mSkyHorizon);
            EXPECT_EQ(constants.mSkyZenith, world.mSkyZenith);

            EXPECT_EQ(constants.mFogColour, world.mAir.mColour);
            EXPECT_EQ(constants.mFogExtinction, world.mAir.mExtinction);
            EXPECT_EQ(constants.mFogUniform, world.mAir.mUniform) << "the game wrote this nowhere";
            EXPECT_EQ(constants.mFogEdge, world.mAir.mEdge);

            // **The one field that does not pass through, and it is meant not to.** What the shader
            // is told is where the surface actually is, and the surface is placed a hair under its
            // nominal level so that ground authored at sea level is not fighting it —
            // `WATER_TIE_BREAK` says why. The two have to move together or the shader's idea of the
            // water and the water disagree.
            EXPECT_EQ(constants.mWaterLevel, world.mWaterLevel - Shaders::WATER_TIE_BREAK);
            EXPECT_EQ(constants.mTime, world.mSeconds) << "the game wrote this nowhere either";

            EXPECT_EQ(constants.mClouds.mOpacity, world.mClouds.mOpacity);
            EXPECT_EQ(constants.mClouds.mLit, world.mClouds.mLit);
            EXPECT_EQ(constants.mClouds.mShadowed, world.mClouds.mShadowed);
            EXPECT_EQ(constants.mClouds.mBlend, world.mClouds.mBlend);
            EXPECT_EQ(constants.mClouds.mScroll, world.mClouds.mScroll);
            EXPECT_EQ(constants.mClouds.mBearing, world.mClouds.mBearing);
            EXPECT_EQ(constants.mClouds.mNextBearing, world.mClouds.mNextBearing);
            EXPECT_EQ(constants.mClouds.mCover, world.mClouds.mCover);
            EXPECT_EQ(constants.mClouds.mAltitude, world.mClouds.mAltitude);
            EXPECT_EQ(constants.mClouds.mPerTile, world.mClouds.mPerTile);
            EXPECT_EQ(constants.mClouds.mCurvature, world.mClouds.mCurvature);
            EXPECT_EQ(constants.mClouds.mRings, world.mClouds.mRings);
            EXPECT_EQ(constants.mClouds.mTexture, world.mClouds.mTexture);
            EXPECT_EQ(constants.mClouds.mNext, world.mClouds.mNext);

            EXPECT_EQ(constants.mStars.mFade, world.mStars.mFade);
            EXPECT_EQ(constants.mStars.mTurn, world.mStars.mTurn);
            EXPECT_EQ(constants.mStars.mTexture, world.mStars.mTexture);

            for (std::size_t moon = 0; moon < world.mMoons.size(); ++moon)
            {
                const MoonPlacement& placed = world.mMoons[moon];
                const Rtx::Shaders::MoonDisc& disc = constants.mMoons[moon];

                EXPECT_EQ(disc.mDirection, placed.mDirection) << "moon " << moon;
                EXPECT_EQ(disc.mRight, placed.mRight) << "moon " << moon;
                EXPECT_EQ(disc.mUp, placed.mUp) << "moon " << moon;
                EXPECT_EQ(disc.mColour, placed.mColour) << "moon " << moon;
                EXPECT_EQ(disc.mAngularRadius, placed.mAngularRadius) << "moon " << moon;
                EXPECT_EQ(disc.mPhaseAngle, placed.mPhaseAngle) << "moon " << moon;
                EXPECT_EQ(disc.mAlpha, placed.mAlpha) << "moon " << moon;
                EXPECT_EQ(disc.mFace, static_cast<std::uint32_t>(placed.mFace)) << "moon " << moon;
            }

            // **And the two moons are not one moon written twice**, which is what an index carried
            // through the loop by mistake would look like and what every field above would still
            // pass under.
            EXPECT_NE(constants.mMoons[0].mAlpha, constants.mMoons[1].mAlpha);
            EXPECT_NE(constants.mMoons[0].mDirection, constants.mMoons[1].mDirection);
        }

        /// A blend that is not a number is not a sky.
        ///
        /// **The bug this is here for turned the game's sky black and left the harness's alone.**
        /// `Weather::cloudBlendFactor` divides the transition by `Clouds_Maximum_Percent`, and the
        /// shipped fallbacks record none for ash or blight — so a transition into either handed back
        /// a NaN. The rasterizer survives one, because a NaN opacity draws nothing and the sky it
        /// already had stays; a tracer mixes its whole sky by it and gets a NaN back, which is black.
        ///
        /// **And `std::clamp` does not catch it**, which is the part worth a test rather than a
        /// comment: it asks whether the value is *outside* the range, both comparisons are false for
        /// a NaN, and it hands the NaN straight back. The boundary has to ask the question the other
        /// way round.
        TEST(RtxSkyBuilderTest, aCloudBlendThatIsNotANumberComesOutAsNoBlendAtAll)
        {
            SkyContent textures;
            textures.mClouds.fill(Rtx::sNoIndex);
            textures.mClouds[Rtx::Shaders::WEATHER_CLEAR] = 3;
            textures.mClouds[Rtx::Shaders::WEATHER_RAIN] = 5;
            textures.mShell = sShell;

            const osg::Vec3f north(0.0f, 1.0f, 0.0f);
            const auto deck = [&](float blend) {
                return describeClouds(Rtx::Shaders::WEATHER_CLEAR, Rtx::Shaders::WEATHER_RAIN, blend, sLight, north,
                    north, 0.0f, textures);
            };

            EXPECT_EQ(deck(std::numeric_limits<float>::quiet_NaN()).mBlend, 0.0f) << "a NaN is no crossing";
            EXPECT_EQ(deck(-1.0f).mBlend, 0.0f) << "and neither is anything under nought";
            EXPECT_EQ(deck(7.0f).mBlend, 1.0f) << "or over one";
            EXPECT_EQ(deck(0.25f).mBlend, 0.25f) << "while a real one is passed through untouched";

            // The rest of the deck still says what it says, so the guard is on the blend and not a
            // bail-out that would have taken the clouds with it.
            EXPECT_EQ(deck(std::numeric_limits<float>::quiet_NaN()).mTexture, 3u);
            EXPECT_EQ(deck(std::numeric_limits<float>::quiet_NaN()).mNext, 5u);
            EXPECT_GT(deck(std::numeric_limits<float>::quiet_NaN()).mOpacity, 0.0f);
        }

        /// A weather the content files give no cloud texture has no deck, rather than a grey one.
        ///
        /// Ash and blight name none in the shipped fallbacks, and Solstheim's two name files the
        /// archives do not hold — and an unreadable texture is drawn as the stand-in, which is an
        /// opaque mid grey and over a deck is the entire sky.
        TEST(RtxSkyBuilderTest, aWeatherWithNoCloudTextureGetsNoDeck)
        {
            SkyContent textures;
            textures.mClouds.fill(Rtx::sNoIndex);
            textures.mClouds[Rtx::Shaders::WEATHER_CLEAR] = 3;
            textures.mShell = sShell;

            EXPECT_EQ(textures.cloudsOf(Rtx::Shaders::WEATHER_CLEAR), 3u);
            EXPECT_EQ(textures.cloudsOf(Rtx::Shaders::WEATHER_ASHSTORM), Rtx::Shaders::NO_TEXTURE);
            EXPECT_EQ(textures.cloudsOf(Rtx::Shaders::WEATHER_COUNT + 4u), Rtx::Shaders::NO_TEXTURE)
                << "and an index past the ten is not a lookup";

            const osg::Vec3f north(0.0f, 1.0f, 0.0f);
            const Rtx::Shaders::CloudDeck none = describeClouds(Rtx::Shaders::WEATHER_ASHSTORM,
                Rtx::Shaders::WEATHER_ASHSTORM, 0.0f, sLight, north, north, 0.0f, textures);

            EXPECT_EQ(none.mOpacity, 0.0f) << "nothing to draw, said the way an interior says it";
            EXPECT_EQ(none.mTexture, Rtx::Shaders::NO_TEXTURE);

            // And a sky whose mesh gave up no shape has nowhere to hang one, whatever sheet the
            // weather names.
            SkyContent unhung = textures;
            unhung.mShell = Rtx::CloudShell{};
            EXPECT_EQ(describeClouds(Rtx::Shaders::WEATHER_CLEAR, Rtx::Shaders::WEATHER_CLEAR, 0.0f, sLight, north,
                          north, 0.0f, unhung)
                          .mOpacity,
                0.0f);
        }

        /// A deck is lit by what stands over it, and its own body is what keeps the sun off its base.
        ///
        /// **The engine paints its deck and this one lights it.** `Sky::cloudColour` adds an eighth
        /// to a display-encoded fog, which read as light is a third again over a clear day and eight
        /// times over the same weather's night — so a painted deck arrived at midnight eight times
        /// the sky it covers. Lit, it is `CLOUD_TRANSMISSION` of whatever reaches it, whatever hour
        /// that is.
        ///
        /// A sun of 8, 4, 2 straight overhead, a sky mean of 0.4, 0.8, 1.2, and a moon a third of
        /// the way up delivering 0.4, 0.8, 1.2 to a face square to it:
        ///
        ///     sky   0.4 * 0.25                    = 0.1
        ///     sun   8 * 1 * 0.25 / pi             = 0.636620
        ///     moon  0.4 * 0.5 * 0.25 / pi         = 0.015915
        ///
        /// so red comes to 0.752535 lit and 0.1 shadowed, and the other two follow their own terms.
        TEST(RtxSkyBuilderTest, aDeckIsLitByWhatStandsOverIt)
        {
            const osg::Vec3f skyMean(0.4f, 0.8f, 1.2f);

            Rtx::Sun sun;
            sun.mPosition = osg::Vec3f(0.0f, 0.0f, 1.0f);
            sun.mIrradiance = osg::Vec3f(8.0f, 4.0f, 2.0f);

            std::array<Rtx::MoonPlacement, 2> moons{};
            moons[0].mDirection = osg::Vec3f(0.0f, std::sqrt(0.75f), 0.5f);
            moons[0].mIrradiance = osg::Vec3f(0.4f, 0.8f, 1.2f);

            const Rtx::DeckLight day = Rtx::deckLight(sun, skyMean, moons);

            EXPECT_NEAR(day.mShadowed.x(), 0.1f, 1.0e-6f);
            EXPECT_NEAR(day.mShadowed.y(), 0.2f, 1.0e-6f);
            EXPECT_NEAR(day.mShadowed.z(), 0.3f, 1.0e-6f);

            EXPECT_NEAR(day.mLit.x(), 0.752535f, 1.0e-5f);
            EXPECT_NEAR(day.mLit.y(), 0.550141f, 1.0e-5f);
            EXPECT_NEAR(day.mLit.z(), 0.506901f, 1.0e-5f);

            // **At night the two differ by the moons alone**, which is the case the whole change is
            // for: no sun over the layer, and a deck that is a quarter of the sky it hides.
            sun.mIrradiance = osg::Vec3f();
            const Rtx::DeckLight night = Rtx::deckLight(sun, skyMean, moons);

            EXPECT_EQ(night.mShadowed, day.mShadowed);
            EXPECT_NEAR(night.mLit.x() - night.mShadowed.x(), 0.015915f, 1.0e-5f);

            // A moon under the horizon delivers nothing to a layer over it, and needs no test of its
            // own to say so — the cosine does it.
            moons[0].mDirection = osg::Vec3f(0.0f, std::sqrt(0.75f), -0.5f);
            const Rtx::DeckLight down = Rtx::deckLight(sun, skyMean, moons);

            EXPECT_EQ(down.mLit, down.mShadowed);
            EXPECT_EQ(down.mShadowed, day.mShadowed);
        }

        /// The shape the deck hangs on is the mesh's, and it is passed through untouched.
        TEST(RtxSkyBuilderTest, theLayersOwnShapeReachesTheShader)
        {
            SkyContent textures;
            textures.mClouds.fill(Rtx::sNoIndex);
            textures.mClouds[Rtx::Shaders::WEATHER_CLEAR] = 3;
            textures.mShell = sShell;

            const osg::Vec3f north(0.0f, 1.0f, 0.0f);
            const Rtx::Shaders::CloudDeck deck = describeClouds(
                Rtx::Shaders::WEATHER_CLEAR, Rtx::Shaders::WEATHER_CLEAR, 0.0f, sLight, north, north, 0.0f, textures);

            EXPECT_EQ(deck.mCurvature, sShell.mCurvature);
            EXPECT_EQ(deck.mRings, sShell.mRings);

            // The mesh's height in tiles reaches the shader as a tile's own width, which is what a
            // chosen altitude turns it into — and it keeps the mesh's sign, because the sheet's `v`
            // runs the other way and dropping that mirrors every cloud.
            EXPECT_EQ(deck.mPerTile, sShell.mTiles / Rtx::sCloudAltitude);
            EXPECT_EQ(deck.mAltitude, Rtx::sCloudAltitude);
            EXPECT_LT(deck.mPerTile.y(), 0.0f);
        }

        /// Each sheet is turned by its own weather's storm, and the turn is the storm itself.
        ///
        /// **The engine turns each of its two cloud meshes separately**, so a transition into an
        /// ashstorm drives the sheet ahead off Red Mountain while the one overhead still runs due
        /// north. Turning a crossing back by that angle wants its cosine and its sine, and for a
        /// unit direction measured from north those are the direction's own two components,
        /// swapped — so no angle is taken and none is undone.
        TEST(RtxSkyBuilderTest, eachSheetIsTurnedByItsOwnWeathersStorm)
        {
            SkyContent textures;
            textures.mClouds.fill(Rtx::sNoIndex);
            textures.mClouds[Rtx::Shaders::WEATHER_CLEAR] = 3;
            textures.mClouds[Rtx::Shaders::WEATHER_RAIN] = 5;
            textures.mShell = sShell;

            const osg::Vec3f north(0.0f, 1.0f, 0.0f);
            const osg::Vec3f east(1.0f, 0.0f, 0.0f);

            const Rtx::Shaders::CloudDeck deck = describeClouds(
                Rtx::Shaders::WEATHER_CLEAR, Rtx::Shaders::WEATHER_RAIN, 0.5f, sLight, north, east, 0.0f, textures);

            EXPECT_EQ(deck.mBearing, osg::Vec2f(1.0f, 0.0f)) << "due north is no turn at all";
            EXPECT_EQ(deck.mNextBearing, osg::Vec2f(0.0f, 1.0f)) << "and due east is a quarter of one";

            // **A direction nobody stated is zero, and a bearing of zero collapses the whole sheet
            // onto one texel.** `WeatherResult` names the weather ahead's storm only while one is
            // arriving, and leaves the field where the last transition left it otherwise.
            const Rtx::Shaders::CloudDeck settled = describeClouds(Rtx::Shaders::WEATHER_CLEAR,
                Rtx::Shaders::WEATHER_RAIN, 0.5f, sLight, north, osg::Vec3f(), 0.0f, textures);

            EXPECT_EQ(settled.mNextBearing, osg::Vec2f(1.0f, 0.0f)) << "which reads as due north";
        }

        /// The level a sheet's texels are read against crosses with the sheet, and falls back with it.
        ///
        /// **The fall-back is the half worth a test.** The shader samples the weather ahead only
        /// where that weather names a sheet, and reads the near one twice where it does not — so a
        /// mean carried half way toward a weather that draws nothing would read every texel of the
        /// near sheet against a level no sheet has, and lift or drop the whole deck by it.
        TEST(RtxSkyBuilderTest, theLevelASheetIsReadAgainstCrossesWithTheSheet)
        {
            SkyContent textures;
            textures.mClouds.fill(Rtx::sNoIndex);
            textures.mClouds[Rtx::Shaders::WEATHER_CLEAR] = 3;
            textures.mClouds[Rtx::Shaders::WEATHER_RAIN] = 5;
            textures.mShell = sShell;
            textures.mCloudMean[Rtx::Shaders::WEATHER_CLEAR] = 0.4f;
            textures.mCloudMean[Rtx::Shaders::WEATHER_RAIN] = 0.2f;

            const osg::Vec3f north(0.0f, 1.0f, 0.0f);
            const auto deck = [&](std::uint32_t next, float blend) {
                return describeClouds(Rtx::Shaders::WEATHER_CLEAR, next, blend, sLight, north, north, 0.0f, textures);
            };

            EXPECT_EQ(deck(Rtx::Shaders::WEATHER_RAIN, 0.0f).mMean, 0.4f);
            EXPECT_EQ(deck(Rtx::Shaders::WEATHER_RAIN, 1.0f).mMean, 0.2f);

            // A quarter of the way across: `0.75 * 0.4 + 0.25 * 0.2`.
            EXPECT_NEAR(deck(Rtx::Shaders::WEATHER_RAIN, 0.25f).mMean, 0.35f, 1.0e-6f);

            // Ash names no sheet in the shipped fallbacks, so the shader reads the clear one at both
            // ends of that crossing and this stays the clear one's whatever the blend says.
            EXPECT_EQ(deck(Rtx::Shaders::WEATHER_ASHSTORM, 0.5f).mMean, 0.4f);
            EXPECT_EQ(textures.meanOf(Rtx::Shaders::WEATHER_COUNT + 4u), 0.0f)
                << "and an index past the ten is not a lookup";
        }

        /// The stars go out when the weather keeps them in, and the sheet is not even named then.
        TEST(RtxSkyBuilderTest, aWeatherThatHidesTheSunHidesTheStarsWithIt)
        {
            SkyContent textures;
            textures.mClouds.fill(Rtx::sNoIndex);
            textures.mNight.mField = 8;
            textures.mNight.mTile = 0.9f;
            textures.mNight.mHorizon = 0.4f;

            // Full night, clear weather: all of the sheet.
            EXPECT_EQ(describeStars(1.0f, 1.0f, 0.0f, textures).mFade, 1.0f);
            EXPECT_EQ(describeStars(1.0f, 1.0f, 0.0f, textures).mTexture, 8u);

            // A thunderstorm's `Glare_View` is nought, and under one there are no stars at all.
            EXPECT_EQ(describeStars(1.0f, 0.0f, 0.0f, textures).mFade, 0.0f);
            EXPECT_EQ(describeStars(1.0f, 0.0f, 0.0f, textures).mTexture, Rtx::Shaders::NO_TEXTURE)
                << "and a sheet nobody can see is one nothing has to sample";

            // Nor by day, whatever the weather is doing.
            EXPECT_EQ(describeStars(0.0f, 1.0f, 0.0f, textures).mFade, 0.0f);

            // Half out is half out, and the roll is carried whatever the fade came to.
            EXPECT_EQ(describeStars(0.5f, 1.0f, 2.5f, textures).mFade, 0.5f);
            EXPECT_EQ(describeStars(0.5f, 1.0f, 2.5f, textures).mTurn, 2.5f);

            // **The scale and the fade come off the mesh and are passed through**, which is the
            // whole reason they are fields and not constants: a replaced night sky changes them.
            EXPECT_EQ(describeStars(1.0f, 1.0f, 0.0f, textures).mTile, 0.9f);
            EXPECT_EQ(describeStars(1.0f, 1.0f, 0.0f, textures).mHorizon, 0.4f);

            // And a mesh that gave up no scale draws nothing, rather than dividing by it.
            SkyContent unread;
            unread.mClouds.fill(Rtx::sNoIndex);
            unread.mNight.mField = 8;
            EXPECT_EQ(describeStars(1.0f, 1.0f, 0.0f, unread).mTexture, Rtx::Shaders::NO_TEXTURE);
        }

        /// The camera's half is left exactly as it was found.
        ///
        /// **The two halves of a frame meet in one struct and neither may write the other's.** The
        /// camera is built first — `mOrigin` is what the storm's direction is asked of — so a world
        /// that reset it would aim the ashstorm from wherever the last frame stood.
        TEST(RtxFrameWorldTest, theWorldLeavesTheCameraAlone)
        {
            Rtx::Shaders::VisibilityConstants constants{};
            constants.mOrigin = osg::Vec3f(1.0f, 2.0f, 3.0f);
            constants.mCamera.mForward = osg::Vec3f(0.0f, 1.0f, 0.0f);
            constants.mCamera.mRight = osg::Vec3f(1.0f, 0.0f, 0.0f);
            constants.mCamera.mUp = osg::Vec3f(0.0f, 0.0f, 1.0f);
            constants.mCamera.mWidth = 1280;
            constants.mCamera.mHeight = 720;
            constants.mNear = 1.0f;
            constants.mFar = 12000.0f;
            constants.mCamera.mSpreadAngle = 0.001f;
            constants.mFrame = 42;
            constants.mDelight = 0.5f;
            constants.mShowAlbedo = 1;
            constants.mTransparentBackground = 1;

            applyWorld(distinct(), constants);

            EXPECT_EQ(constants.mOrigin, osg::Vec3f(1.0f, 2.0f, 3.0f));
            EXPECT_EQ(constants.mCamera.mForward, osg::Vec3f(0.0f, 1.0f, 0.0f));
            EXPECT_EQ(constants.mCamera.mRight, osg::Vec3f(1.0f, 0.0f, 0.0f));
            EXPECT_EQ(constants.mCamera.mUp, osg::Vec3f(0.0f, 0.0f, 1.0f));
            EXPECT_EQ(constants.mCamera.mWidth, 1280u);
            EXPECT_EQ(constants.mCamera.mHeight, 720u);
            EXPECT_EQ(constants.mNear, 1.0f);
            EXPECT_EQ(constants.mFar, 12000.0f);
            EXPECT_EQ(constants.mCamera.mSpreadAngle, 0.001f);
            EXPECT_EQ(constants.mFrame, 42u);
            EXPECT_EQ(constants.mDelight, 0.5f);
            EXPECT_EQ(constants.mShowAlbedo, 1u);
            EXPECT_EQ(constants.mTransparentBackground, 1u);
        }

        /// A default world is a frame with no sky in it, which is what an interface trace wants.
        TEST(RtxFrameWorldTest, aWorldNobodyFilledDrawsNoSunAndNoMoons)
        {
            Rtx::Shaders::VisibilityConstants constants{};
            applyWorld(FrameWorld{}, constants);

            // **One statement of "no sun", and the disc reads it too.** There is no second field to
            // leave set: a frame with no irradiance draws no disc, casts nothing and lights no haze.
            EXPECT_EQ(constants.mSunIrradiance, osg::Vec3f()) << "no sun, and so no disc of one";
            EXPECT_EQ(constants.mSunDiscColour, osg::Vec3f(1.0f, 1.0f, 1.0f)) << "a plain white one when there is";
            EXPECT_EQ(constants.mMoons[0].mAlpha, 0.0f) << "and no moons";
            EXPECT_EQ(constants.mMoons[0].mFace, Rtx::Shaders::NO_TEXTURE) << "and no portrait to draw";
            EXPECT_EQ(constants.mClouds.mOpacity, 0.0f) << "and no deck over it";
            EXPECT_EQ(constants.mClouds.mTexture, Rtx::Shaders::NO_TEXTURE);
            EXPECT_EQ(constants.mStars.mTexture, Rtx::Shaders::NO_TEXTURE) << "and no stars in it";
            EXPECT_EQ(constants.mMoons[1].mAlpha, 0.0f);
            EXPECT_EQ(constants.mFogExtinction, 0.0f) << "and air that costs nothing";
            EXPECT_EQ(constants.mFogEdge, 0.0f) << "and no edge for it to close over";

            // Minus infinity and not zero: zero is sea level, and a frame with no water has to
            // answer "how deep is this point" with never.
            EXPECT_LT(constants.mWaterLevel, -1.0e30f);
        }
    }
}
