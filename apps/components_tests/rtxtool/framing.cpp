#include <algorithm>
#include <cmath>

#include <gtest/gtest.h>

#include <osg/Vec3f>
#include <osg/Vec4f>

#include <components/fallback/fallback.hpp>
#include <components/rtx/camera.hpp>
#include <components/rtx/error.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/shaders/visibility.h>
#include <components/weather/downpour.hpp>

#include <apps/rtxtool/framing.hpp>
#include <apps/rtxtool/placement.hpp>

namespace RtxTool
{
    namespace
    {
        /// A trace at 1280x720 presented at 1920x1080, which is what the harness actually runs.
        constexpr Rtx::FrameExtents sExtents{
            .mRenderWidth = 1280,
            .mRenderHeight = 720,
            .mOutputWidth = 1920,
            .mOutputHeight = 1080,
        };

        /// Out where Morrowind's cells are, because that is where the two ways of naming a direction
        /// stop agreeing to the bit.
        Placement makePlacement()
        {
            return Placement{ .mOrigin = { -19216.0f, -14896.0f, 160.0f }, .mTarget = { -19424.0f, -12960.0f, 60.0f } };
        }

        Framing makeFraming()
        {
            Framing framing = Framing::lookingFrom(makePlacement());
            framing.mFieldOfView = 60.0f;
            framing.mFar = 200000.0f;
            return framing;
        }

        /// Two points and their difference name the same camera.
        ///
        /// **The equivalence the consolidation rests on.** `shot` and `bench` held a target and
        /// called `Rtx::makeCamera`; they now hand a direction to `Rtx::makeCameraAlong` through
        /// `lookingFrom`. `makeCamera` is documented as subtracting and delegating, so this is
        /// asserting that it still does — the day it grows a step of its own, a screenshot quietly
        /// changes and nothing else says so.
        TEST(RtxFramingTest, lookingFromTwoPointsIsTheSameCameraAsAimingAtOne)
        {
            const Placement placement = makePlacement();

            const Rtx::Shaders::VisibilityConstants aimed = Rtx::makeCamera(
                placement.mOrigin, placement.mTarget, 60.0f, sExtents.mRenderWidth, sExtents.mRenderHeight, 200000.0f);
            const Rtx::Shaders::VisibilityConstants framed = makeFrameConstants(makeFraming(), sExtents);

            EXPECT_EQ(framed.mOrigin, aimed.mOrigin);
            EXPECT_EQ(framed.mCamera.mForward, aimed.mCamera.mForward);
            EXPECT_EQ(framed.mCamera.mRight, aimed.mCamera.mRight);
            EXPECT_EQ(framed.mCamera.mUp, aimed.mCamera.mUp);
            EXPECT_EQ(framed.mFar, aimed.mFar);
        }

        /// The trace's extent and not the presented one is what the camera is built for.
        ///
        /// A camera built for 1920x1080 and traced at 1280x720 has the wrong aspect and the wrong
        /// per-pixel ray spread, which reads as a stretched image rather than as an error.
        TEST(RtxFramingTest, theCameraIsBuiltForWhatTheTraceRunsAtRatherThanWhatIsPresented)
        {
            const Rtx::Shaders::VisibilityConstants framed = makeFrameConstants(makeFraming(), sExtents);
            const Rtx::Shaders::VisibilityConstants square = makeFrameConstants(makeFraming(),
                Rtx::FrameExtents{
                    .mRenderWidth = 720, .mRenderHeight = 720, .mOutputWidth = 1920, .mOutputHeight = 1080 });

            // 16:9 spreads the same vertical half-angle over 16/9 as much width; 1:1 does not.
            EXPECT_NEAR(framed.mCamera.mRight.length() / square.mCamera.mRight.length(), 16.0f / 9.0f, 1e-5f);
        }

        /// Every switch reaches the constants, and each one changes the frame.
        ///
        /// **The whole point of one block instead of three.** A field that quietly stopped being
        /// copied would leave a command rendering something else, which is exactly the drift that
        /// made `bench` the only one of the three not honouring `--albedo`.
        TEST(RtxFramingTest, eachSwitchReachesTheConstantsAndChangesThem)
        {
            const Rtx::Shaders::VisibilityConstants plain = makeFrameConstants(makeFraming(), sExtents);
            EXPECT_EQ(plain.mShowAlbedo, 0u);

            Framing shown = makeFraming();
            shown.mShowAlbedo = true;
            EXPECT_EQ(makeFrameConstants(shown, sExtents).mShowAlbedo, 1u);

            Framing lit = makeFraming();
            lit.mDelight = 0.25f;
            EXPECT_EQ(makeFrameConstants(lit, sExtents).mDelight, 0.25f);
            EXPECT_NE(makeFrameConstants(lit, sExtents).mDelight, plain.mDelight);

            Framing sequenced = makeFraming();
            sequenced.mFrame = 42;
            EXPECT_EQ(makeFrameConstants(sequenced, sExtents).mFrame, 42u);

            Framing distant = makeFraming();
            distant.mFar = 10000.0f;
            EXPECT_EQ(makeFrameConstants(distant, sExtents).mFar, 10000.0f);
            EXPECT_NE(makeFrameConstants(distant, sExtents).mFar, plain.mFar);

            // The lighting goes through `applyLighting`, so one field of it is enough to say the
            // call is made at all.
            Framing moving = makeFraming();
            moving.mLighting.mSeconds = 3.5f;
            moving.mLighting.mWaterLevel = -12.0f;
            EXPECT_EQ(makeFrameConstants(moving, sExtents).mTime, 3.5f);
            EXPECT_EQ(makeFrameConstants(moving, sExtents).mWaterLevel, -12.0f - Rtx::Shaders::WATER_TIE_BREAK)
                << "the level the shader is given is where the surface is, which is a hair under the nominal one";
            EXPECT_NE(makeFrameConstants(moving, sExtents).mTime, plain.mTime);
        }

        /// The clock and the weather move the sky and leave the cell alone.
        ///
        /// **What the window's `,` `.` and `[` `]` turn.** Two weathers have to reach two different
        /// skies or the keys are decoration, and an interior has to come back untouched: its ambient
        /// and its air are its own `AMBI` record, which no hour has a say in.
        ///
        /// The settings below are planted so this stands up on its own — an allowed key the map
        /// never received answers middle grey, and two weathers left unseeded would agree, which
        /// would pass for the wrong reason.
        ///
        /// **But they are not what is asserted against.** `Fallback::Map::init` keeps whichever
        /// value arrives first, and a test elsewhere in this binary opens the real installation and
        /// plants Morrowind's own — so which of the two a key holds depends on the order the suite
        /// ran in. Every expectation here is therefore against what `makeDaylight` says the same
        /// weather is, which is `relight`'s actual contract and true of either source.
        TEST(RtxFramingTest, theClockAndTheWeatherMoveTheSkyAndLeaveTheCellAlone)
        {
            Fallback::Map::init({
                { "Weather_Sunrise_Time", "6" },
                { "Weather_Sunset_Time", "18" },
                { "Weather_Sunset_Duration", "2" },
                { "Weather_Clear_Land_Fog_Day_Depth", "0.4" },
                { "Weather_Clear_Land_Fog_Night_Depth", "0.8" },
                { "Weather_Clear_Wind_Speed", "0.3" },
                { "Weather_Clear_Sky_Day_Color", "100,150,200" },
                { "Weather_Clear_Sun_Day_Color", "255,255,255" },
                { "Weather_Overcast_Land_Fog_Day_Depth", "0.9" },
                { "Weather_Overcast_Land_Fog_Night_Depth", "0.9" },
                { "Weather_Overcast_Wind_Speed", "0.7" },
                { "Weather_Overcast_Sky_Day_Color", "80,80,80" },
                { "Weather_Overcast_Sun_Day_Color", "120,120,120" },
            });

            CellLighting outdoors{ .mWaterLevel = -32.0f, .mOutdoors = true };

            relight(outdoors, "Clear", 0, 12.0f);
            EXPECT_EQ(outdoors.mWeather, Rtx::Shaders::WEATHER_CLEAR);
            EXPECT_FLOAT_EQ(outdoors.mWindSpeed, Weather::windSpeed("Clear"));
            EXPECT_EQ(outdoors.mDaylight.mSkyZenith, Rtx::makeDaylight("Clear", 12.0f).mSkyZenith);
            EXPECT_GT(outdoors.mDaylight.mSun.mIrradiance.x(), 0.0f) << "noon has a sun";

            const osg::Vec3f noon = outdoors.mDaylight.mSun.mIrradiance;

            // **Midnight is the same weather at another hour, and its sun is dimmer rather than
            // absent.** A content file records a night colour for the sun and the engine reads it
            // straight off the ramp, so a harness that switched the sun off at midnight lit its
            // nights differently from the game.
            relight(outdoors, "Clear", 0, 0.0f);
            EXPECT_EQ(outdoors.mDaylight.mSun.mIrradiance, Rtx::makeDaylight("Clear", 0.0f).mSun.mIrradiance);
            EXPECT_NE(outdoors.mDaylight.mSun.mIrradiance, noon) << "midnight is not noon";
            EXPECT_EQ(outdoors.mWeather, Rtx::Shaders::WEATHER_CLEAR) << "the hour is not the weather";

            // And another weather is another sky, at the same hour.
            relight(outdoors, "Overcast", 5, 12.0f);
            EXPECT_EQ(outdoors.mWeather, Rtx::Shaders::WEATHER_OVERCAST);
            EXPECT_EQ(outdoors.mDay, 5);
            EXPECT_FLOAT_EQ(outdoors.mWindSpeed, Weather::windSpeed("Overcast"));
            EXPECT_EQ(outdoors.mDaylight.mSkyZenith, Rtx::makeDaylight("Overcast", 12.0f).mSkyZenith);
            EXPECT_GT(outdoors.mFog.mExtinction, 0.0f);

            // **And the two are different skies**, whichever file the numbers came out of, which is
            // what says the weather key does anything at all.
            EXPECT_NE(Rtx::makeDaylight("Clear", 12.0f).mSkyZenith, Rtx::makeDaylight("Overcast", 12.0f).mSkyZenith);

            // The cell's own half is left where it was: nothing here reads the water.
            EXPECT_EQ(outdoors.mWaterLevel, -32.0f);

            // **And a transition is between the two rather than either of them**, which is the one
            // thing this tool never ran: the blend the shader carries was exercised only in the
            // game. The engine mixes the results — every colour, and the fog's recorded depth before
            // it becomes an extinction — so halfway is halfway on each of them.
            CellLighting turning{ .mWaterLevel = -32.0f, .mOutdoors = true };
            relight(turning, "Clear", "Overcast", 0.5f, 0, 12.0f);

            EXPECT_EQ(turning.mWeather, Rtx::Shaders::WEATHER_CLEAR);
            EXPECT_EQ(turning.mNextWeather, Rtx::Shaders::WEATHER_OVERCAST);
            EXPECT_FLOAT_EQ(turning.mWeatherBlend, 0.5f);

            // **Halfway in the space the file records, not in the one the renderer works in**, which
            // is where the engine mixes: `calculateTransitionResult` lerps colours it never decodes,
            // and the decode happens once at the end. Averaging the two decoded colours instead
            // lands somewhere else entirely — sRGB is furthest from linear in exactly the middle,
            // which is where a fifty per cent blend sits — so the midpoint here is re-encoded before
            // it is compared. The inverse transfer is spelled out rather than shared: a test that
            // called the same helper the code does would pass however wrong that helper was.
            const auto encode = [](float linear) {
                return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
            };

            const osg::Vec3f clear = Rtx::makeDaylight("Clear", 12.0f).mSkyZenith;
            const osg::Vec3f overcast = Rtx::makeDaylight("Overcast", 12.0f).mSkyZenith;
            for (int channel = 0; channel < 3; ++channel)
            {
                const osg::Vec4f half(0.5f * (encode(clear[channel]) + encode(overcast[channel])), 0.0f, 0.0f, 0.0f);
                EXPECT_NEAR(turning.mDaylight.mSkyZenith[channel], Rtx::decodeColour(half).x(), 1e-5f)
                    << "channel " << channel;

                // And it really is between them, which the re-encoding above could otherwise hide.
                EXPECT_GT(turning.mDaylight.mSkyZenith[channel], std::min(clear[channel], overcast[channel]))
                    << "channel " << channel;
                EXPECT_LT(turning.mDaylight.mSkyZenith[channel], std::max(clear[channel], overcast[channel]))
                    << "channel " << channel;
            }

            // Either end of the mix is the weather at that end, exactly.
            CellLighting ends{ .mWaterLevel = -32.0f, .mOutdoors = true };
            relight(ends, "Clear", "Overcast", 0.0f, 0, 12.0f);
            EXPECT_EQ(ends.mDaylight.mSkyZenith, clear);
            relight(ends, "Clear", "Overcast", 1.0f, 0, 12.0f);
            EXPECT_EQ(ends.mDaylight.mSkyZenith, overcast);

            // And the wind crosses with them, which is one of the numbers the engine blends too.
            EXPECT_FLOAT_EQ(turning.mWindSpeed, 0.5f * (Weather::windSpeed("Clear") + Weather::windSpeed("Overcast")));

            // **An interior has no sky for a clock to move.** Every field comes back as it went in,
            // including the weather it was never under.
            const CellLighting room{ .mAmbient = osg::Vec3f(0.1f, 0.2f, 0.3f), .mWaterLevel = -8.0f };
            CellLighting moved = room;
            relight(moved, "Overcast", 5, 3.0f);
            EXPECT_EQ(moved.mAmbient, room.mAmbient);
            EXPECT_EQ(moved.mWaterLevel, room.mWaterLevel);
            EXPECT_EQ(moved.mWeather, room.mWeather);
            EXPECT_EQ(moved.mDay, room.mDay);
            EXPECT_EQ(moved.mWindSpeed, room.mWindSpeed);
        }

        /// A camera with no basis says so rather than filling the image with NaN.
        TEST(RtxFramingTest, aDirectionNothingCanBeBuiltFromIsRefused)
        {
            Framing nowhere = makeFraming();
            nowhere.mForward = osg::Vec3f(0.0f, 0.0f, 0.0f);
            EXPECT_THROW(makeFrameConstants(nowhere, sExtents), Rtx::Error) << "no direction to look along";

            Framing upward = makeFraming();
            upward.mForward = osg::Vec3f(0.0f, 0.0f, 1.0f);
            EXPECT_THROW(makeFrameConstants(upward, sExtents), Rtx::Error) << "straight up has no roll";

            // A viewpoint whose `pos` and `look` are the same point reaches the first of those.
            const Placement still{ .mOrigin = { 1.0f, 2.0f, 3.0f }, .mTarget = { 1.0f, 2.0f, 3.0f } };
            EXPECT_THROW(makeFrameConstants(Framing::lookingFrom(still), sExtents), Rtx::Error);
        }
    }
}
