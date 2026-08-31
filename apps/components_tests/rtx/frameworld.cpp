#include <gtest/gtest.h>

#include <cmath>

#include <components/rtx/cloudshell.hpp>
#include <components/rtx/frameworld.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/shaders/visibility.h>
#include <components/rtx/skybuilder.hpp>

namespace Rtx
{
    namespace
    {
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
                    .mLift = 2.75f,
                    .mWind = 0.45f,
                    .mEdge = 24576.0f },
                .mWaterLevel = -37.5f,
                .mSeconds = 12.25f,
                .mRainOnWater = 0.35f,

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
            EXPECT_EQ(constants.mFogLift, world.mAir.mLift);
            EXPECT_EQ(constants.mFogEdge, world.mAir.mEdge);

            // **The wind blows the way the deck drifts**, because there is one wind over a
            // landscape. The deck holds the cosine and sine of its turn from north, so a bearing of
            // (0.8, 0.6) is a storm driving along (0.6, 0.8) — and 0.45 of a wind on it is
            // (0.27, 0.36), not the pair the deck holds.
            EXPECT_FLOAT_EQ(constants.mFogWind.x(), 0.27f);
            EXPECT_FLOAT_EQ(constants.mFogWind.y(), 0.36f);

            // And the sea runs the same way, as a unit heading: the deck's `(0.8, 0.6)` is a turn,
            // whose heading is `(0.6, 0.8)`.
            EXPECT_FLOAT_EQ(constants.mSeaHeading.x(), 0.6f);
            EXPECT_FLOAT_EQ(constants.mSeaHeading.y(), 0.8f);

            // A world with no deck over it — a room — has no wind, and its water runs as the tiles
            // were drawn rather than nowhere.
            FrameWorld still = world;
            still.mClouds.mBearing = osg::Vec2f();
            Shaders::VisibilityConstants becalmed = constants;
            applyWorld(still, becalmed);
            EXPECT_EQ(becalmed.mSeaHeading, osg::Vec2f(1.0f, 0.0f));

            // **The one field that does not pass through, and it is meant not to.** What the shader
            // is told is where the surface actually is, and the surface is placed a hair under its
            // nominal level so that ground authored at sea level is not fighting it —
            // `WATER_TIE_BREAK` says why. The two have to move together or the shader's idea of the
            // water and the water disagree.
            EXPECT_EQ(constants.mWaterLevel, world.mWaterLevel - Shaders::WATER_TIE_BREAK);
            EXPECT_EQ(constants.mTime, world.mSeconds) << "the game wrote this nowhere either";
            EXPECT_EQ(constants.mRainOnWater, world.mRainOnWater);

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
                EXPECT_FLOAT_EQ(disc.mLimb, std::sin(placed.mAngularRadius)) << "moon " << moon;
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

            return textures;
        }

        /// A reading whose numbers are distinct, so a field taken from the wrong one shows.
        WorldReading reading()
        {
            return WorldReading{
                .mDaylight = Daylight{
                    .mSun = { .mPosition = osg::Vec3f(0.0f, 0.0f, 1.0f),
                        .mIrradiance = osg::Vec3f(8.0f, 4.0f, 2.0f),
                        .mDiscColour = osg::Vec3f(1.0f, 0.8f, 0.65f) },
                    .mSunAloft = { .mPosition = osg::Vec3f(0.0f, 0.0f, 1.0f),
                        .mIrradiance = osg::Vec3f(9.0f, 5.0f, 3.0f) },
                    .mSkyHorizon = osg::Vec3f(0.21f, 0.22f, 0.23f),
                    .mSkyZenith = osg::Vec3f(0.31f, 0.32f, 0.33f),
                    .mAmbient = osg::Vec3f(0.11f, 0.12f, 0.13f),
                    .mStarFade = 1.0f,
                    .mFog = { .mColour = osg::Vec3f(0.41f, 0.42f, 0.43f), .mExtinction = 1.5e-4f },
                },
                .mOutdoors = true,
                .mFogFromSky = true,
                .mGlare = 1.0f,
                .mStarRoll = 0.125f,
                .mCloudRoll = 0.25f,
                .mSky = skyWithSheets(),
                .mWeather = Rtx::Shaders::WEATHER_CLEAR,
                .mNextWeather = Rtx::Shaders::WEATHER_CLEAR,
                .mWaterLevel = -37.5f,
                .mSeconds = 12.25f,
                .mRainOnWater = 0.35f,
            };
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
            room.mFogFromSky = false;

            // A moon a caller left behind: full in the sky, painted, and lighting.
            room.mMoons[0] = MoonPlacement{ .mDirection = osg::Vec3f(0.0f, 0.0f, 1.0f),
                .mRight = osg::Vec3f(1.0f, 0.0f, 0.0f),
                .mUp = osg::Vec3f(0.0f, 1.0f, 0.0f),
                .mAngularRadius = 0.1f,
                .mAlpha = 1.0f,
                .mFace = 7,
                .mIrradiance = osg::Vec3f(0.05f, 0.05f, 0.06f) };

            const FrameWorld world = describeWorld(room);

            EXPECT_EQ(world.mClouds.mTexture, Rtx::Shaders::NO_TEXTURE);
            EXPECT_EQ(world.mStars.mTexture, Rtx::Shaders::NO_TEXTURE);
            EXPECT_EQ(world.mSkyPatches[0].mTexture, Rtx::Shaders::NO_TEXTURE);

            // **Both halves, because a moon is a disc and a light.** `VisibilityVariant` folds its
            // kernel away on the pair, so leaving either would keep a room tracing shadow rays at a
            // body that is not over it.
            EXPECT_EQ(world.mMoons[0].mAlpha, 0.0f) << "no moon drawn in a room";
            EXPECT_EQ(world.mMoons[0].mIrradiance, osg::Vec3f()) << "and none lighting one";
            EXPECT_EQ(world.mMoons[0].mFace, Rtx::sNoIndex) << "and no portrait to draw";

            EXPECT_EQ(world.mAmbientFromSky, 0.0f);
            EXPECT_EQ(world.mSkyFill, osg::Vec3f());

            EXPECT_EQ(world.mAir.mColour, room.mDaylight.mFog.mColour)
                << "a room has no dome for its air to take a colour from";

            // The same reading out of doors keeps every one of them, so the rows above are the
            // flag's doing rather than the assembly dropping a moon it was handed.
            room.mOutdoors = true;
            const FrameWorld open = describeWorld(room);
            EXPECT_EQ(open.mMoons[0].mAlpha, 1.0f);
            EXPECT_EQ(open.mMoons[0].mIrradiance, osg::Vec3f(0.05f, 0.05f, 0.06f));
        }

        /// An exterior's air is the record's hue under the dome's own mean, and a quasi-exterior's
        /// is the record as it stands.
        ///
        /// **Two readings that differ in one flag and must not come out alike.** A quasi-exterior is
        /// outdoors — it has weather, a deck and stars — and its fog is written in the cell rather
        /// than in the weather, so mixing the dome into it would state a colour the content never
        /// wrote.
        TEST(RtxFrameWorldTest, onlyAnAirLitByTheDomeTakesItsColourFromIt)
        {
            const WorldReading open = reading();

            WorldReading quasi = open;
            quasi.mFogFromSky = false;

            const FrameWorld outside = describeWorld(open);
            const FrameWorld inside = describeWorld(quasi);

            EXPECT_NE(outside.mAir.mColour, inside.mAir.mColour) << "one flag, and it decided nothing";

            EXPECT_EQ(inside.mAir.mColour, open.mDaylight.mFog.mColour);

            const SkyBudget budget = skyBudget(open.mDaylight.mSkyHorizon, open.mDaylight.mSkyZenith,
                describeStars(open.mDaylight.mStarFade, open.mGlare, open.mStarRoll, open.mSky).mGlow,
                open.mDaylight.mAmbient);
            EXPECT_EQ(outside.mAir.mColour, fogColour(budget.mMean, open.mDaylight.mFog.mColour));

            // And both are outdoors, which is what makes them the same case but for the air.
            EXPECT_EQ(outside.mAmbientFromSky, 1.0f);
            EXPECT_EQ(inside.mAmbientFromSky, 1.0f);
            EXPECT_NE(inside.mClouds.mTexture, Rtx::Shaders::NO_TEXTURE);
            EXPECT_NE(inside.mStars.mTexture, Rtx::Shaders::NO_TEXTURE);
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
            dark.mDaylight.mSun = Sun{};
            dark.mDaylight.mSunAloft = Sun{};
            dark.mDaylight.mStarFade = 0.0f;

            WorldReading starry = dark;
            starry.mDaylight.mStarFade = 1.0f;

            const FrameWorld night = describeWorld(dark);
            const FrameWorld stars = describeWorld(starry);

            EXPECT_GT(stars.mStars.mGlow.x(), night.mStars.mGlow.x()) << "the fade decided nothing";
            EXPECT_GT(stars.mClouds.mShadowed.x(), night.mClouds.mShadowed.x())
                << "the deck was lit out of a sky the stars had not been counted into";
        }
    }
}
