#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Math>
#include <osg/Vec2d>
#include <osg/Vec2f>
#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <components/rtx/cloudshell.hpp>
#include <components/rtx/fogbuilder.hpp>
#include <components/rtx/frameworld.hpp>
#include <components/rtx/moonbuilder.hpp>
#include <components/rtx/nightsky.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>
#include <components/rtx/shaders/look.h>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/shaders/sky.h>
#include <components/rtx/shaders/visibility.h>
#include <components/rtx/skybuilder.hpp>
#include <components/rtx/skylight.hpp>

#include "extractor/fixture.hpp"

namespace Rtx
{
    namespace
    {
        /// A rain box with one quad of our own under it, which is all the walk can tell from a storm.
        ///
        /// **What `mirrorPrecipitation` is handed is a node, an eye and whether it is submerged**, so
        /// a group that drops nothing is enough to ask both of its questions, and needs no content
        /// files to build.
        osg::ref_ptr<osg::Group> makeFalling()
        {
            osg::ref_ptr<osg::Geometry> drop = new osg::Geometry;
            drop->setVertexArray(
                Testing::makePositions({ { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } }));
            drop->addPrimitiveSet(Testing::makeTriangles({ 0, 1, 2 }));

            osg::ref_ptr<osg::Geode> holder = new osg::Geode;
            holder->addDrawable(drop);

            osg::ref_ptr<osg::Group> falling = new osg::Group;
            falling->addChild(holder);
            return falling;
        }

        /// A sky to describe a world under, with every sheet the assembly can reach for.
        SkyContent skyWithSheets()
        {
            SkyContent textures;
            textures.mClouds.fill(Rtx::sNoIndex);
            textures.mClouds[Rtx::Shaders::WEATHER_CLEAR] = 3;
            textures.mCloudMean[Rtx::Shaders::WEATHER_CLEAR] = 0.435f;
            textures.mShell = Rtx::CloudShell{
                .mTiles = osg::Vec2f(0.75f, -0.75f), .mCurvature = 0.06f, .mRings = osg::Vec3f(1.0f, 1.5f, 2.0f)
            };
            textures.mNight.mField = 5;
            textures.mNight.mTile = 0.25f;
            textures.mNight.mGlow = osg::Vec3f(0.02f, 0.03f, 0.04f);
            textures.mNight.mPatches[0] = Rtx::NightSky::Patch{
                .mTexture = 13, .mDirection = osg::Vec3f(0.0f, 1.0f, 0.0f), .mAngularRadius = 0.2f
            };

            return textures;
        }

        /// A reading whose numbers are distinct, so a field taken from the wrong one shows.
        /// `describeWorld` for a test that reads the constants, with the options it wrote handed back.
        FrameOptions describe(const WorldReading& reading, FogDrift& drift, Shaders::VisibilityConstants& constants)
        {
            FrameOptions options;
            describeWorld(reading, drift, constants, options);
            return options;
        }

        WorldReading reading()
        {
            return WorldReading{
                .mDaylight = Daylight{
                    .mLight = { .mSun = { .mPosition = osg::Vec3f(0.0f, 0.0f, 1.0f),
                                    .mIrradiance = osg::Vec3f(8.0f, 4.0f, 2.0f),
                                    .mDiscColour = osg::Vec3f(1.0f, 0.8f, 0.65f) },
                        .mSunAloft = { .mPosition = osg::Vec3f(0.0f, 0.0f, 1.0f),
                            .mIrradiance = osg::Vec3f(9.0f, 5.0f, 3.0f) },
                        .mAmbient = osg::Vec3f(0.11f, 0.12f, 0.13f),
                        .mExposureBias = 0.75f },
                    .mSkyHorizon = osg::Vec3f(0.21f, 0.22f, 0.23f),
                    .mSkyZenith = osg::Vec3f(0.31f, 0.32f, 0.33f),
                    .mStarFade = 1.0f,
                    .mFog = { .mColour = osg::Vec3f(0.41f, 0.42f, 0.43f), .mExtinction = 1.5e-4f },
                },
                .mOutdoors = true,
                .mGlare = 1.0f,
                .mStarRoll = 0.125f,
                .mSky = skyWithSheets(),
                .mClouds = Rtx::CloudCrossing{
                    .mWeather = Rtx::Shaders::WEATHER_CLEAR,
                    .mNext = Rtx::Shaders::WEATHER_CLEAR,
                    .mScroll = 0.25f,
                },
                .mWaterLevel = -37.5f,
                .mSeconds = 12.25f,
                .mSkySeconds = 47.5,
                .mRainOnWater = 0.35f,
                .mShelterHeight = 8992.0f,
                .mSunGlare = SunGlare{
                    .mColour = osg::Vec3f(1.0f, 0.745f, 0.306f),
                    .mAngleMax = 0.5236f,
                    .mStrength = 0.125f,
                },
            };
        }

        /// The same, with every field the air and the moons carry filled in as well.
        ///
        /// **A bearing of `(0.8, 0.6)`, which the deck takes from a storm running `(0.6, 0.8)`** —
        /// `bearingOf` holds the cosine and sine of the turn from north, which for a unit direction
        /// is its components the other way round.
        WorldReading distinctReading()
        {
            WorldReading read = reading();

            read.mDaylight.mFog.mUniform = 0.75f;
            read.mDaylight.mFog.mLift = 2.75f;
            read.mDaylight.mFog.mWind = 0.45f;
            read.mDaylight.mFog.mEdge = 24576.0f;
            read.mClouds.mDirection = osg::Vec3f(0.6f, 0.8f, 0.0f);
            read.mClouds.mNextDirection = osg::Vec3f(0.96f, 0.28f, 0.0f);
            read.mClouds.mBlend = 0.25f;

            read.mMoons[0] = MoonPlacement{
                .mDirection = osg::Vec3f(0.0f, 0.0f, 1.0f),
                .mRight = osg::Vec3f(1.0f, 0.0f, 0.0f),
                .mUp = osg::Vec3f(0.0f, 1.0f, 0.0f),
                .mAngularRadius = 0.1676f,
                .mPhaseAngle = 0.25f,
                .mAlpha = 0.5f,
                .mFace = 7,
                .mColour = osg::Vec3f(0.0332f, 0.0099f, 0.0123f),
            };
            read.mMoons[1] = MoonPlacement{
                .mDirection = osg::Vec3f(0.0f, 1.0f, 0.0f),
                .mRight = osg::Vec3f(-1.0f, 0.0f, 0.0f),
                .mUp = osg::Vec3f(0.0f, 0.0f, 1.0f),
                .mAngularRadius = 0.0719f,
                .mPhaseAngle = 2.5f,
                .mAlpha = 0.25f,
                .mFace = 9,
                .mColour = osg::Vec3f(0.0440f, 0.0373f, 0.0295f),
            };

            return read;
        }

        /// A drop's own travel is its fall, and nothing falls where the eye is under water.
        ///
        /// **The box of drops carries no translation of its own**, so its particles are placed about
        /// the origin and the eye is what stands them in the world. Anchoring the walk anywhere else
        /// makes every sprite's motion between two frames the eye's step as well as its fall, which
        /// is a reprojection of the wrong thing — and the drops would slide with the camera.
        ///
        /// **And the walk stops entirely under water.** The sky manager freezes the drops where
        /// they stand and leaves what to draw to whoever is drawing; walked anyway, the ones the
        /// surface was crossed with hang in the air for as long as the eye stays under it.
        TEST(RtxFrameWorldTest, dropsAreStoodAtTheEyeAndNoneIsWalkedUnderWater)
        {
            const osg::ref_ptr<osg::Group> falling = makeFalling();
            const osg::Vec3f eye(1000.0f, -2000.0f, 300.0f);

            SceneDesc scene;
            SceneExtractor extractor(scene);
            mirrorPrecipitation(extractor, falling, eye, false, 0, 0);

            ASSERT_EQ(scene.placements().getCounts().mPlaced, 1u) << "the drop was not walked at all";
            EXPECT_EQ(Testing::placedAt(scene, 0), eye) << "the drops were stood somewhere other than the eye";

            // Held still where the eye is submerged, which is a walk that does not happen rather
            // than geometry that is hidden.
            SceneDesc under;
            SceneExtractor beneath(under);
            mirrorPrecipitation(beneath, falling, eye, true, 0, 0);

            EXPECT_EQ(under.placements().getCounts().mPlaced, 0u);

            // And a world with no weather over it at all is the third case the one call answers.
            SceneDesc dry;
            SceneExtractor none(dry);
            mirrorPrecipitation(none, nullptr, eye, false, 0, 0);

            EXPECT_EQ(dry.placements().getCounts().mPlaced, 0u);
        }

        /// Every number the world decides reaches the constants, and reaches the right one.
        ///
        /// **The test the two paths never had.** The game and the harness each used to write these
        /// twenty-odd fields themselves, which is how the sea's clock came to be filled by one and
        /// left at zero by the other, and how the game's interiors came to run the outdoor fog field.
        /// One conversion is what fixed that; this is what says the conversion is complete.
        ///
        /// Written against a zeroed frame so that a field `describeWorld` forgets stays zero and
        /// fails here, rather than quietly carrying whatever the camera left behind.
        TEST(RtxFrameWorldTest, everyNumberTheWorldDecidesReachesTheFrame)
        {
            const WorldReading read = distinctReading();

            Rtx::Shaders::VisibilityConstants constants{};
            FogDrift drift;
            FrameOptions options;
            describeWorld(read, drift, constants, options);

            const Skylight& light = read.mDaylight.mLight;
            EXPECT_EQ(constants.mSun.mDirection, light.mSun.mPosition);
            EXPECT_EQ(constants.mSun.mIrradiance, light.mSun.mIrradiance);
            EXPECT_EQ(constants.mSunDiscColour, light.mSun.mDiscColour);
            EXPECT_EQ(constants.mAmbient, light.mAmbient);
            EXPECT_EQ(constants.mSkyHorizon, read.mDaylight.mSkyHorizon);
            EXPECT_EQ(constants.mSkyZenith, read.mDaylight.mSkyZenith);

            EXPECT_EQ(constants.mFogExtinction, read.mDaylight.mFog.mExtinction);
            EXPECT_EQ(constants.mFogUniform, read.mDaylight.mFog.mUniform) << "the game wrote this nowhere";
            EXPECT_EQ(constants.mFogLift, read.mDaylight.mFog.mLift);
            EXPECT_EQ(constants.mFogEdge, read.mDaylight.mFog.mEdge);

            // The one number a world decides that no reading carries: a world's frame is the one
            // the reconstruction follows, so it is the one that rates its bounces.
            EXPECT_EQ(constants.mBounceRate, Shaders::BOUNCE_RATE);

            // **The wind blows the way the deck drifts**, because there is one wind over a
            // landscape. The deck holds the cosine and sine of its turn from north, so a storm
            // driving along (0.6, 0.8) is a bearing of (0.8, 0.6) — and 0.45 of a wind on the
            // storm's own heading is (0.27, 0.36), not the pair the deck holds.
            //
            // **And the frame is handed the distance blown, never the wind times the clock.** The
            // first reading has no earlier one to measure from, so the fog has gone nowhere yet;
            // one second on, at `FOG_GALE` units a second of wind, it has gone (0.27, 0.36) × 1400.
            // A second of the water's clock carries no air: the wind blows on the sky's. What the
            // frame is handed is that distance and the churn, reduced — `fogOffsets` of both.
            const auto expectOffsets
                = [](const Shaders::VisibilityConstants& frame, const osg::Vec2d& carried, double skySeconds) {
                      const std::array<osg::Vec3f, Shaders::FOG_SCALES> offsets = fogOffsets(carried, skySeconds);
                      for (std::size_t scale = 0; scale < offsets.size(); ++scale)
                          EXPECT_EQ(frame.mFogOffsets[scale], offsets[scale]) << "scale " << scale;
                  };

            EXPECT_FLOAT_EQ(constants.mClouds.mBearing.x(), 0.8f);
            EXPECT_FLOAT_EQ(constants.mClouds.mBearing.y(), 0.6f);
            EXPECT_EQ(drift.get(), osg::Vec2d());
            expectOffsets(constants, osg::Vec2d(), read.mSkySeconds);

            WorldReading waterLater = read;
            waterLater.mSeconds = read.mSeconds + 1.0;
            Shaders::VisibilityConstants waterMoved{};
            describe(waterLater, drift, waterMoved);
            EXPECT_EQ(drift.get(), osg::Vec2d());

            WorldReading later = read;
            later.mSkySeconds = read.mSkySeconds + 1.0;
            Shaders::VisibilityConstants blown{};
            describe(later, drift, blown);
            EXPECT_NEAR(drift.get().x(), 378.0, 1e-3);
            EXPECT_NEAR(drift.get().y(), 504.0, 1e-3);
            expectOffsets(blown, drift.get(), later.mSkySeconds);

            // And the sea runs the same way, as a unit heading.
            EXPECT_FLOAT_EQ(constants.mSeaHeading.x(), 0.6f);
            EXPECT_FLOAT_EQ(constants.mSeaHeading.y(), 0.8f);

            // A world with no deck over it — a room — has no wind, and its water runs as the tiles
            // were drawn rather than nowhere. The fog keeps the distance it was blown, because a
            // door is not a wind: what would move on the way through it is the whole of the drift.
            WorldReading still = later;
            still.mOutdoors = false;
            still.mSkySeconds = later.mSkySeconds + 1.0;
            Shaders::VisibilityConstants becalmed{};
            describe(still, drift, becalmed);
            EXPECT_EQ(becalmed.mSeaHeading, osg::Vec2f(1.0f, 0.0f));
            EXPECT_NEAR(drift.get().x(), 378.0, 1e-3);
            EXPECT_NEAR(drift.get().y(), 504.0, 1e-3);
            expectOffsets(becalmed, drift.get(), still.mSkySeconds);

            // **The one field that does not pass through, and it is meant not to.** What the shader
            // is told is where the surface actually is, and the surface is placed a hair under its
            // nominal level so that ground authored at sea level is not fighting it —
            // `WATER_TIE_BREAK` says why. The two have to move together or the shader's idea of the
            // water and the water disagree.
            EXPECT_EQ(constants.mWaterLevel, read.mWaterLevel - Shaders::WATER_TIE_BREAK);
            EXPECT_EQ(constants.mWaterTime, splitSeconds(read.mSeconds)) << "the game wrote this nowhere either";
            EXPECT_EQ(constants.mRainOnWater, read.mRainOnWater);
            EXPECT_EQ(constants.mShelterHeight, read.mShelterHeight);
            // And beside the constants, what the display chain and the ripples take: the glare as the
            // reading stated it, the sky's clock, and the hour's bias.
            EXPECT_EQ(options.mGlare.mColour, read.mSunGlare.mColour);
            EXPECT_EQ(options.mGlare.mAngleMax, read.mSunGlare.mAngleMax);
            EXPECT_EQ(options.mGlare.mStrength, read.mSunGlare.mStrength);
            EXPECT_EQ(options.mSkySeconds, read.mSkySeconds);
            EXPECT_EQ(options.mExposureBias, light.mExposureBias);

            // The deck and the stars come out of the builders both hosts share, and this is the one
            // place that says the frame is handed what those built rather than a second reading.
            const Shaders::StarField stars
                = describeStars(read.mDaylight.mStarFade, read.mGlare, read.mStarRoll, read.mSky);
            EXPECT_EQ(constants.mStars.mFade, stars.mFade);
            EXPECT_EQ(constants.mStars.mTurn, stars.mTurn);
            EXPECT_EQ(constants.mStars.mTexture, stars.mTexture);
            EXPECT_EQ(constants.mStars.mGlow, stars.mGlow);

            EXPECT_EQ(constants.mClouds.mBlend, read.mClouds.mBlend);
            EXPECT_EQ(constants.mClouds.mTexture, read.mSky.cloudsOf(read.mClouds.mWeather));
            EXPECT_EQ(constants.mClouds.mScroll, read.mClouds.mScroll);
            EXPECT_EQ(constants.mClouds.mCurvature, read.mSky.mShell.mCurvature);
            EXPECT_EQ(constants.mClouds.mRings, read.mSky.mShell.mRings);
            EXPECT_FLOAT_EQ(constants.mClouds.mNextBearing.x(), 0.28f);
            EXPECT_FLOAT_EQ(constants.mClouds.mNextBearing.y(), 0.96f);

            EXPECT_NE(constants.mSkyPatches[0].mTexture, Rtx::Shaders::NO_TEXTURE) << "no sheet reached the sky";

            for (std::size_t moon = 0; moon < read.mMoons.size(); ++moon)
            {
                const MoonPlacement& placed = read.mMoons[moon];
                const Rtx::Shaders::MoonDisc& disc = constants.mMoons[moon];

                EXPECT_EQ(disc.mSource.mDirection, placed.mDirection) << "moon " << moon;
                EXPECT_EQ(disc.mRight, placed.mRight) << "moon " << moon;
                EXPECT_EQ(disc.mUp, placed.mUp) << "moon " << moon;
                EXPECT_EQ(disc.mColour, placed.mColour) << "moon " << moon;
                EXPECT_FLOAT_EQ(disc.mSource.mLimb, std::sin(placed.mAngularRadius)) << "moon " << moon;
                EXPECT_EQ(disc.mSource.mIrradiance, osg::componentMultiply(placed.mIrradiance, placed.mPaint))
                    << "moon " << moon;
                EXPECT_EQ(disc.mPhaseAngle, placed.mPhaseAngle) << "moon " << moon;
                EXPECT_EQ(disc.mAlpha, placed.mAlpha) << "moon " << moon;
                EXPECT_EQ(disc.mFace, static_cast<std::uint32_t>(placed.mFace)) << "moon " << moon;
            }

            // **And the two moons are not one moon written twice**, which is what an index carried
            // through the loop by mistake would look like and what every field above would still
            // pass under.
            EXPECT_NE(constants.mMoons[0].mAlpha, constants.mMoons[1].mAlpha);
            EXPECT_NE(constants.mMoons[0].mSource.mDirection, constants.mMoons[1].mSource.mDirection);

            // **The day's gain lifts the whole sky and nothing else.** Four, because scaling by a
            // power of two is exact through every sum and product the sky's terms go through, so each
            // lifted field is its unlifted self times four to the bit: the sun, the ambient, the dome,
            // what the sky fills with, the air's colour, the deck's light, the stars and the moons'
            // light. The frame carries the gain for the one thing drawn off a constant, a moon's face.
            WorldReading day = read;
            day.mDaylight.mLight.mDaylightGain = 4.0f;
            Shaders::VisibilityConstants lifted{};
            FogDrift dayDrift;
            describe(day, dayDrift, lifted);

            EXPECT_EQ(constants.mDaylightGain, 1.0f);
            EXPECT_EQ(lifted.mDaylightGain, 4.0f);
            EXPECT_EQ(lifted.mSun.mIrradiance, constants.mSun.mIrradiance * 4.0f);
            EXPECT_EQ(lifted.mAmbient, constants.mAmbient * 4.0f);
            EXPECT_EQ(lifted.mSkyHorizon, constants.mSkyHorizon * 4.0f);
            EXPECT_EQ(lifted.mSkyZenith, constants.mSkyZenith * 4.0f);
            EXPECT_EQ(lifted.mSkyFill, constants.mSkyFill * 4.0f);
            EXPECT_EQ(lifted.mFogColour, constants.mFogColour * 4.0f);
            EXPECT_EQ(lifted.mClouds.mLit, constants.mClouds.mLit * 4.0f);
            EXPECT_EQ(lifted.mClouds.mShadowed, constants.mClouds.mShadowed * 4.0f);
            EXPECT_EQ(lifted.mStars.mFade, constants.mStars.mFade * 4.0f);
            EXPECT_EQ(lifted.mStars.mGlow, constants.mStars.mGlow * 4.0f);
            for (std::size_t moon = 0; moon < read.mMoons.size(); ++moon)
            {
                EXPECT_EQ(lifted.mMoons[moon].mSource.mIrradiance, constants.mMoons[moon].mSource.mIrradiance * 4.0f)
                    << "moon " << moon;
                EXPECT_EQ(lifted.mMoons[moon].mColour, constants.mMoons[moon].mColour) << "a face's paint is not light";
            }

            EXPECT_EQ(lifted.mSunDiscColour, constants.mSunDiscColour) << "a colour and not a light";
            EXPECT_EQ(lifted.mFogExtinction, constants.mFogExtinction) << "the air is no thicker by day";
            ASSERT_GT(constants.mSkyFill.x() + constants.mClouds.mLit.x() + constants.mStars.mGlow.x(), 0.0f)
                << "nothing the gain reaches was lit, so the gain was not tried";
        }

        /// The glare fader's amount is `SunGlareCallback`'s own line: the strength, faded to nothing
        /// linearly over `Angle_Max` off the eye's axis, and nothing at all for a frame with no
        /// fader in it — whatever the eye is looking at.
        TEST(RtxFrameWorldTest, theGlareFaderFadesLinearlyOffTheEyesAxis)
        {
            Shaders::VisibilityConstants frame{};
            frame.mCamera.mForward = osg::Vec3f(0.0f, 1.0f, 0.0f);
            SunGlare fader{ .mAngleMax = osg::DegreesToRadians(30.0f), .mStrength = 0.5f };

            // Straight at it, ten degrees off, thirty off and past thirty: one, two thirds, nought
            // and nought of the strength.
            const auto amountAt = [&](float degrees) {
                const float off = osg::DegreesToRadians(degrees);
                frame.mSun
                    = Shaders::sunSource(osg::Vec3f(std::sin(off), std::cos(off), 0.0f), osg::Vec3f(1.0f, 1.0f, 1.0f));
                return fader.amountFor(frame);
            };

            EXPECT_FLOAT_EQ(amountAt(0.0f), 0.5f);
            EXPECT_NEAR(amountAt(10.0f), 0.5f * (1.0f - 10.0f / 30.0f), 1e-6f);
            EXPECT_NEAR(amountAt(30.0f), 0.0f, 1e-6f);
            EXPECT_EQ(amountAt(45.0f), 0.0f);

            fader.mStrength = 0.0f;
            EXPECT_EQ(amountAt(0.0f), 0.0f);
        }

        /// A clock is handed over as two floats whose sum is it, to a nanosecond after a hundred hours.
        ///
        /// A hundred hours is 360,000 s, where a float steps by 1/32: the nearest float to
        /// 360,000.123 is 360,000.125, and the second float carries the -0.002 the first overshot by.
        /// Nought splits into two noughts, and a clock a float holds whole leaves nothing over.
        TEST(RtxFrameWorldTest, aClockIsHandedOverAsTwoFloatsWhoseSumIsIt)
        {
            EXPECT_EQ(splitSeconds(0.0), osg::Vec2f());
            EXPECT_EQ(splitSeconds(12.25), osg::Vec2f(12.25f, 0.0f));

            constexpr double hundredHours = 360000.123;
            const osg::Vec2f split = splitSeconds(hundredHours);
            EXPECT_EQ(split.x(), 360000.125f);
            EXPECT_NEAR(static_cast<double>(split.x()) + static_cast<double>(split.y()), hundredHours, 1e-9);
        }

        /// Each scale of the fog is read from where the churn and the turned drift moved it, as a
        /// fraction of its own tile, and a hundred hours in that fraction is still exact.
        ///
        /// By hand: at one second of the sky and no drift, the coarse tile is `FOG_TILE`, 7200 units,
        /// and its churn of (11, 7, 0) a second is (11, 7, 0) / 7200 of it. Carried 100 units along +x
        /// at nought seconds, the middle scale reads from upwind, (-100, 0), which its 3-4-5 turn
        /// takes to (-80, -60): negative, so the fraction is one less 80 and 60 of its tile,
        /// 7200 / 2.27. The coarse scale takes the same drift unturned.
        TEST(RtxFrameWorldTest, theFogIsReadFromWhereTheChurnAndTheDriftMovedItReducedToItsTile)
        {
            const std::array<osg::Vec3f, Shaders::FOG_SCALES> churned = fogOffsets(osg::Vec2d(), 1.0);
            EXPECT_FLOAT_EQ(churned[0].x(), 11.0f / 7200.0f);
            EXPECT_FLOAT_EQ(churned[0].y(), 7.0f / 7200.0f);
            EXPECT_EQ(churned[0].z(), 0.0f);

            const float middleTile = Shaders::FOG_TILE / Shaders::FOG_LACUNARITY;
            const std::array<osg::Vec3f, Shaders::FOG_SCALES> carried = fogOffsets(osg::Vec2d(100.0, 0.0), 0.0);
            EXPECT_FLOAT_EQ(carried[0].x(), 1.0f - 100.0f / 7200.0f);
            EXPECT_EQ(carried[0].y(), 0.0f);
            EXPECT_FLOAT_EQ(carried[1].x(), 1.0f - 80.0f / middleTile);
            EXPECT_FLOAT_EQ(carried[1].y(), 1.0f - 60.0f / middleTile);
            EXPECT_EQ(carried[1].z(), 0.0f);

            // **A hundred hours in, against long double.** A float holding the churn's
            // 19 × 360,000 = 6.8 million units steps by half a unit, and the drift after a storm of
            // that length is further still; the fraction is what the device is handed instead.
            constexpr double seconds = 360000.123;
            const osg::Vec2d drift(123456.75, -98765.5);
            const std::array<osg::Vec3f, Shaders::FOG_SCALES> far = fogOffsets(drift, seconds);

            const std::array<osg::Vec3f, Shaders::FOG_SCALES> churns{ Shaders::FOG_CHURN_COARSE,
                Shaders::FOG_CHURN_MIDDLE, Shaders::FOG_CHURN_FINE };
            const std::array<osg::Vec2f, Shaders::FOG_SCALES> turns{ osg::Vec2f(1.0f, 0.0f), Shaders::FOG_TURN_MIDDLE,
                Shaders::FOG_TURN_FINE };
            float tile = Shaders::FOG_TILE;
            for (std::size_t scale = 0; scale < far.size(); ++scale)
            {
                if (scale > 0)
                    tile /= Shaders::FOG_LACUNARITY;

                const long double c = turns[scale].x();
                const long double s = turns[scale].y();
                const long double t = seconds;
                const std::array<long double, 3> moved{
                    -c * drift.x() + s * drift.y() + churns[scale].x() * t,
                    -s * drift.x() - c * drift.y() + churns[scale].y() * t,
                    churns[scale].z() * t,
                };
                for (std::size_t axis = 0; axis < moved.size(); ++axis)
                {
                    const long double turned = moved[axis] / tile;
                    const long double expected = turned - std::floor(turned);
                    EXPECT_NEAR(far[scale][static_cast<unsigned>(axis)], static_cast<float>(expected), 1e-6f)
                        << "scale " << scale << ", axis " << axis;
                }
            }
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
            constants.mShow = Shaders::SHOW_ALBEDO;
            constants.mTransparentBackground = 1;

            FogDrift drift;
            describe(distinctReading(), drift, constants);

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
            EXPECT_EQ(constants.mShow, Shaders::SHOW_ALBEDO);
            EXPECT_EQ(constants.mTransparentBackground, 1u);
        }

        /// A reading nobody filled is a frame with no sky in it, which is what an interface trace
        /// wants.
        TEST(RtxFrameWorldTest, aWorldNobodyFilledDrawsNoSunAndNoMoons)
        {
            Rtx::Shaders::VisibilityConstants constants{};
            FogDrift drift;
            describe(WorldReading{}, drift, constants);

            // **One statement of "no sun", and the disc reads it too.** There is no second field to
            // leave set: a frame with no irradiance draws no disc, casts nothing and lights no haze.
            EXPECT_EQ(constants.mSun.mIrradiance, osg::Vec3f()) << "no sun, and so no disc of one";
            EXPECT_EQ(constants.mSunDiscColour, osg::Vec3f(1.0f, 1.0f, 1.0f)) << "a plain white one when there is";
            EXPECT_EQ(constants.mMoons[0].mAlpha, 0.0f) << "and no moons";
            EXPECT_EQ(constants.mMoons[0].mFace, Rtx::Shaders::NO_TEXTURE) << "and no portrait to draw";
            EXPECT_EQ(constants.mClouds.mOpacity, 0.0f) << "and no deck over it";
            EXPECT_EQ(constants.mClouds.mTexture, Rtx::Shaders::NO_TEXTURE);
            EXPECT_EQ(constants.mStars.mTexture, Rtx::Shaders::NO_TEXTURE) << "and no stars in it";
            EXPECT_EQ(constants.mSkyPatches[0].mTexture, Rtx::Shaders::NO_TEXTURE) << "and no sheets across it";
            EXPECT_EQ(constants.mMoons[1].mAlpha, 0.0f);
            EXPECT_EQ(constants.mFogExtinction, 0.0f) << "and air that costs nothing";
            EXPECT_EQ(constants.mFogEdge, 0.0f) << "and no edge for it to close over";

            // Minus infinity and not zero: zero is sea level, and a frame with no water has to
            // answer "how deep is this point" with never.
            EXPECT_LT(constants.mWaterLevel, -1.0e30f);
        }

        /// A room draws no sky at all, and keeps the air its own record states.
        ///
        /// **The whole of what `mOutdoors` decides, asserted in one place.** Each of these used to
        /// be a branch written twice — once in the game and once in the harness — and a room that
        /// drew an outdoor deck is the shape the drift took the last three times. The moons were
        /// the one the game kept: the weather system stops reporting when the player steps inside,
        /// so what it last said was still standing in the frame, lighting through the seams.
        TEST(RtxFrameWorldTest, aRoomDrawsNoDeckNoStarsNoPatchesAndNoMoons)
        {
            WorldReading room = reading();
            room.mOutdoors = false;

            // A moon a caller left behind: full in the sky, painted, and lighting.
            room.mMoons[0] = MoonPlacement{ .mDirection = osg::Vec3f(0.0f, 0.0f, 1.0f),
                .mRight = osg::Vec3f(1.0f, 0.0f, 0.0f),
                .mUp = osg::Vec3f(0.0f, 1.0f, 0.0f),
                .mAngularRadius = 0.1f,
                .mAlpha = 1.0f,
                .mFace = 7,
                .mIrradiance = osg::Vec3f(0.05f, 0.05f, 0.06f) };

            Shaders::VisibilityConstants world{};
            FogDrift drift;
            describe(room, drift, world);

            EXPECT_EQ(world.mClouds.mTexture, Rtx::Shaders::NO_TEXTURE);
            EXPECT_EQ(world.mStars.mTexture, Rtx::Shaders::NO_TEXTURE);
            EXPECT_EQ(world.mSkyPatches[0].mTexture, Rtx::Shaders::NO_TEXTURE);

            // **Both halves, because a moon is a disc and a light.** `VisibilityVariant` folds its
            // kernel away on the pair, so leaving either would keep a room tracing shadow rays at a
            // body that is not over it.
            EXPECT_EQ(world.mMoons[0].mAlpha, 0.0f) << "no moon drawn in a room";
            EXPECT_EQ(world.mMoons[0].mSource.mIrradiance, osg::Vec3f()) << "and none lighting one";
            EXPECT_EQ(world.mMoons[0].mFace, Rtx::Shaders::NO_TEXTURE) << "and no portrait to draw";

            EXPECT_EQ(world.mAmbientFromSky, 0.0f);
            EXPECT_EQ(world.mSkyFill, osg::Vec3f());

            EXPECT_EQ(world.mFogColour, room.mDaylight.mFog.mColour)
                << "a room has no dome for its air to take a colour from";

            // The same reading out of doors keeps every one of them, so the rows above are the
            // flag's doing rather than the assembly dropping a moon it was handed.
            room.mOutdoors = true;
            Shaders::VisibilityConstants open{};
            describe(room, drift, open);
            EXPECT_EQ(open.mMoons[0].mAlpha, 1.0f);
            EXPECT_EQ(open.mMoons[0].mSource.mIrradiance, osg::Vec3f(0.05f, 0.05f, 0.06f));
        }

        /// The bias is the light's, and this is the only thing between it and `FrameOptions`.
        ///
        /// **Carried and never derived, because a room is the exception to the rule that would
        /// derive it** — `Skylight::mExposureBias`. So `mOutdoors` must not reach this one: a
        /// reader that held a room at one by testing the flag would hold a lit cellar there too.
        TEST(RtxFrameWorldTest, theExposureBiasIsCarriedFromWhicheverLightTheCellGot)
        {
            WorldReading open = reading();
            open.mDaylight.mLight.mExposureBias = 0.375f;

            Shaders::VisibilityConstants lit{};
            FogDrift drift;
            EXPECT_FLOAT_EQ(describe(open, drift, lit).mExposureBias, 0.375f);

            WorldReading room = open;
            room.mOutdoors = false;
            room.mDaylight.mLight.mExposureBias = 0.625f;

            Shaders::VisibilityConstants inside{};
            EXPECT_FLOAT_EQ(describe(room, drift, inside).mExposureBias, 0.625f)
                << "the flag reached a number that is not its";
        }

        /// Air under a dome is lit by it, and air with no dome over it keeps the colour it was
        /// given.
        ///
        /// **One flag decides it, where there were two.** A quasi-exterior used to carry a second
        /// saying its air was a cell's, and its air is a weather's: the Construction Set greys the
        /// whole `AMBI` record out for a cell that behaves like an exterior. So the dome lights a
        /// canton's air exactly as it lights any other weather's, and the two readings a flag can
        /// still tell apart are the open air and a room's. `RtxReadWorldTest` is where a
        /// quasi-exterior's own air is asserted.
        TEST(RtxFrameWorldTest, airUnderADomeIsLitByIt)
        {
            const WorldReading open = reading();

            WorldReading room = open;
            room.mOutdoors = false;

            Shaders::VisibilityConstants outside{};
            Shaders::VisibilityConstants inside{};
            FogDrift drift;
            describe(open, drift, outside);
            describe(room, drift, inside);

            EXPECT_NE(outside.mFogColour, inside.mFogColour) << "one flag, and it decided nothing";

            const SkyBudget budget = skyBudget(open.mDaylight.mSkyHorizon, open.mDaylight.mSkyZenith,
                describeStars(open.mDaylight.mStarFade, open.mGlare, open.mStarRoll, open.mSky).mGlow,
                open.mDaylight.mLight.mAmbient);
            EXPECT_EQ(outside.mFogColour, fogColour(budget.mMean, open.mDaylight.mFog.mColour));
        }

        /// The deck is lit by the dome the stars are counted into, which is the order this exists
        /// to keep.
        ///
        /// **Brighter stars make a brighter deck, and nothing else in the reading moves.** The star
        /// glow is spent into `SkyBudget::mMean`, the mean lights the deck, and a host that
        /// described its deck before its stars would light it out of a sky one term short.
        TEST(RtxFrameWorldTest, whatTheStarsAddReachesTheDeckThatHangsUnderThem)
        {
            WorldReading dark = reading();
            dark.mDaylight.mLight.mSun = Sun{};
            dark.mDaylight.mLight.mSunAloft = Sun{};
            dark.mDaylight.mStarFade = 0.0f;

            WorldReading starry = dark;
            starry.mDaylight.mStarFade = 1.0f;

            Shaders::VisibilityConstants night{};
            Shaders::VisibilityConstants stars{};
            FogDrift drift;
            describe(dark, drift, night);
            describe(starry, drift, stars);

            EXPECT_GT(stars.mStars.mGlow.x(), night.mStars.mGlow.x()) << "the fade decided nothing";
            EXPECT_GT(stars.mClouds.mShadowed.x(), night.mClouds.mShadowed.x())
                << "the deck was lit out of a sky the stars had not been counted into";
        }
    }
}
