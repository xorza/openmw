#include <gtest/gtest.h>

#include <osg/Vec3f>

#include <components/esm3/loadcell.hpp>
#include <components/rtx/fogbuilder.hpp>
#include <components/rtx/lightbuilder.hpp>
#include <components/settings/values.hpp>

namespace Rtx
{
    namespace
    {
        /// A recorded fog depth becomes the extinction that halves where the original ramp does.
        ///
        /// The original engine fogs *linearly* between `view * (1 - depth)` and `view`, so it is
        /// half gone at `view * (1 - depth / 2)`, while an exponential is half gone at
        /// `ln(2) / sigma`. Over the game's own view range, clear weather's 0.69 is
        ///
        ///   ln(2) / (7168 * 0.655) = 1.4763e-4 per unit,
        ///
        /// which is where the renderer this is ported from arrived by eye at 1.5e-4. Two routes to
        /// one number, and the reason this one is derived rather than copied.
        TEST(RtxFogTest, aRecordedDepthBecomesTheExtinctionThatHalvesWhereTheOriginalRampDoes)
        {
            // Stated rather than read, because the figures below are only the game's if this is.
            ASSERT_EQ(Settings::camera().mViewingDistance, 7168.0f) << "the view range these are against";

            // **Passed rather than assumed**, because outdoors this renderer no longer measures the
            // air against it: the world is built to `distant land cells` and fog tuned to a shorter
            // reach swallows all of it. What is checked here is the conversion, against the range the
            // original engine used, which is still what a room is measured by.
            constexpr float view = 7168.0f;

            EXPECT_NEAR(fogExtinction(0.69f, view), 1.4763e-4f, 1e-8f) << "clear weather";

            // Thicker weather is thicker, by the ratio the ramp itself gives: foggy's depth of 1.0
            // puts the half-way point at half the view range against clear's 0.655 of it.
            EXPECT_NEAR(fogExtinction(1.0f, view) / fogExtinction(0.69f, view), 0.655f / 0.5f, 0.001f)
                << "foggy weather against clear";

            // And a depth of zero is no fog at all rather than a ramp starting at the view distance,
            // which is what the original engine reads it as too.
            EXPECT_EQ(fogExtinction(0.0f, view), 0.0f);

            // **And the reach is what scales it**, which is the whole of §3.4: the same weather over
            // four cells is thinner in exactly that proportion, so ground built that far out is
            // still there to be seen.
            EXPECT_NEAR(fogExtinction(0.69f, 4.0f * 8192.0f) / fogExtinction(0.69f, view), view / 32768.0f, 1e-5f)
                << "the air did not stretch with the world";
        }

        /// A room's air is thinner than the conversion above makes it, and by one fixed number.
        ///
        /// **The ramp and the medium part company indoors** — `sInteriorFogReach` says at length
        /// why, and the short of it is that the ramp is clear across the whole of a room where a
        /// medium cannot be, and that a lit medium is not a colour a pixel is mixed toward. What is
        /// checked here is that the stretch is applied and that it is the *only* thing separating a
        /// room's extinction from the raw conversion, since the game reaches the same number by a
        /// different route and the two may not drift.
        TEST(RtxFogTest, aRoomsAirIsStretchedPastTheRangeItsRecordWasWrittenAgainst)
        {
            // The shipped default of the range the original engine measures a room against, which is
            // what the stretch below was set against and is no longer read from.
            constexpr float view = 7168.0f;

            // **The dial itself, pinned once**, so moving it is a deliberate line and never a
            // surprise. Everything below this is a property that holds at whatever it is set to.
            EXPECT_FLOAT_EQ(sInteriorFogReach, 25.0f * view);

            // A longer range is exactly that much less extinction: an exponential's half-life is
            // precisely the distance it is measured over. A ratio of a fortieth carries about four
            // billionths of float noise, so the bound is two orders above that and still far under
            // any drift a changed rule would cause.
            EXPECT_NEAR(
                fogExtinction(0.69f, sInteriorFogReach) / fogExtinction(0.69f, view), view / sInteriorFogReach, 1e-7f)
                << "a room is thinner than the ramp it came from, by the stretch and by nothing else";

            // The Seyda Neen customs office, whose depth of 0.75 is what the stretch was set
            // against:
            //
            //   sigma = ln(2) / (25 * 7168 * (1 - 0.375)) = 0.693147 / 112000 = 6.1888e-6 per unit
            //
            // against the 1.5472e-4 the unstretched conversion gives, which is what put a tenth of a
            // lamp-lit medium between the eye and a wall seven hundred units away.
            EXPECT_NEAR(fogExtinction(0.75f, sInteriorFogReach), 6.1888e-6f, 1e-10f);

            // **Proportional and not a floor**, so a denser room is still the denser one: foggy's
            // 1.0 against clear's 0.69 keeps exactly the ratio it had before the stretch.
            EXPECT_NEAR(fogExtinction(1.0f, sInteriorFogReach) / fogExtinction(0.69f, sInteriorFogReach),
                fogExtinction(1.0f, view) / fogExtinction(0.69f, view), 1e-5f);

            // And a room the record gives no fog at all still has none, however far it is measured
            // over — the stretch scales an extinction and never creates one.
            EXPECT_EQ(fogExtinction(0.0f, sInteriorFogReach), 0.0f);
        }

        /// A room's air does not move when the player changes how much world they want to see.
        ///
        /// **That was the whole reason for a constant.** The original engine measures a room's ramp
        /// against `viewing distance`, so raising the setting thinned the air in every windowless
        /// cellar in the game — a knob about how much world is built saying how a room feels. The
        /// value is that range's shipped default and it is now written down rather than read.
        TEST(RtxFogTest, aRoomsAirIsWhatTheContentSaidAndNotWhatTheViewDistanceIs)
        {
            const ESM::Cell::AMBIstruct room{ .mFog = 0x00808080, .mFogDensity = 0.75f };
            const auto air = [&room] { return makeRoomLight(room).mFog.mExtinction; };

            const float thick = air();
            EXPECT_NEAR(thick, 6.1888e-6f, 1e-10f) << "the customs office, from its own record alone";

            Settings::camera().mViewingDistance.set(4.0f * 8192.0f);
            EXPECT_FLOAT_EQ(air(), thick) << "a cellar cleared because the sky got bigger";

            Settings::camera().mViewingDistance.set(2048.0f);
            EXPECT_FLOAT_EQ(air(), thick) << "and thickened because it got smaller";

            Settings::camera().mViewingDistance.set(7168.0f);
        }

        /// The air is the sky's own light in the weather's colour, and never brighter than the sky.
        ///
        /// **Normalised by the brightest channel and not by the luminance.** Blight's `Fog Day Color`
        /// is (128, 19, 19): its luminance is a twentieth of its red, so dividing by that made the
        /// red four times the light that lit it. Against the maximum the red is exactly the sky's
        /// red and the other two are a seventh of it, which is a deep red darker than a clear day.
        TEST(RtxFogTest, theAirIsTheSkysLightInTheWeathersColour)
        {
            const osg::Vec3f sky(2.0f, 3.0f, 4.0f);

            // A grey record hands the sky back untouched: every channel is the brightest.
            EXPECT_EQ(fogColour(sky, osg::Vec3f(0.5f, 0.5f, 0.5f)), sky);

            // Blight, as the file records it: 128, 19, 19 over 255.
            const osg::Vec3f blight = fogColour(sky, osg::Vec3f(128.0f, 19.0f, 19.0f) / 255.0f);
            EXPECT_FLOAT_EQ(blight.x(), 2.0f) << "the brightest channel is the sky's own";
            EXPECT_FLOAT_EQ(blight.y(), 3.0f * 19.0f / 128.0f);
            EXPECT_FLOAT_EQ(blight.z(), 4.0f * 19.0f / 128.0f);

            // A record of nothing at all lights nothing rather than dividing by it.
            EXPECT_EQ(fogColour(sky, osg::Vec3f()), osg::Vec3f());
        }

        /// A weather with more fog has fog that reaches higher, and the wind stands it higher still.
        ///
        /// **The record is read twice and the two readings must agree about direction.**
        /// `fogExtinction` takes it as the view-range ramp the original engine wrote, and `fogLift`
        /// takes it as what the field is called — a depth. Foggy records 1.0 by day and 1.9 by
        /// night against clear's 0.69, so its air fills a bay where clear's lies in the hollows.
        ///
        /// **And the wind cannot stand in for the depth.** Bethesda puts foggy's wind at nought, so
        /// a layer driven by wind alone made the weather named foggy the shallowest of the ten.
        TEST(RtxFogTest, aWeatherWithMoreFogStandsItsLayerHigher)
        {
            // Clear by day, dead still: the layer the shader's own constant names.
            EXPECT_FLOAT_EQ(fogLift(0.69f, 0.0f), 1.0f);

            // Foggy by night is 1.9 against clear's 0.69, and it blows at nothing at all.
            EXPECT_FLOAT_EQ(fogLift(1.9f, 0.0f), 1.9f / 0.69f);
            EXPECT_GT(fogLift(1.9f, 0.0f), fogLift(0.69f, 0.0f)) << "a still fog is deeper than a still clear day";

            // A blizzard records 3.0 and blows at 0.9, so both halves push the same way.
            EXPECT_FLOAT_EQ(fogLift(3.0f, 0.9f), 3.0f / 0.69f * (1.0f + 0.9f * sFogWindLift));

            // The wind alone would put foggy under a rainstorm, which is the mistake the depth
            // exists to stop: rain records 0.8 and blows at 0.3.
            EXPECT_GT(fogLift(1.9f, 0.0f), fogLift(0.8f, 0.3f)) << "depth beats wind, which is why both are read";
        }

        /// The open air is measured over the same reach it closes at, and a room closes at nothing.
        ///
        /// **Two elements out of one number, which is why one function builds both.** How thick the
        /// air is and where it becomes opaque are the same question about how much world there is,
        /// and the game and the harness reach a weather by different routes. Each assembling these
        /// fields itself is how the game's air came to carry no edge: the ring where its ground
        /// stops stayed visible while a screenshot of the same hour hid it.
        ///
        /// **Moved rather than read once**, because two numbers that happen to agree at four cells
        /// look exactly like one number until the setting moves. Doubling the reach halves the
        /// extinction and doubles the edge, which only one number can do.
        TEST(RtxFogTest, theOpenAirIsMeasuredOverTheSameReachItClosesAt)
        {
            const osg::Vec3f haze(0.4f, 0.5f, 0.6f);
            constexpr float cell = 8192.0f;

            Settings::rtx().mDistantLandCells.set(4.0f);
            const Fog near = exteriorFog(haze, 0.69f, 0.0f);
            EXPECT_EQ(near.mColour, haze);
            EXPECT_EQ(near.mEdge, 4.0f * cell);
            EXPECT_FLOAT_EQ(near.mExtinction, fogExtinction(0.69f, 4.0f * cell));

            // **Banked out of doors**, which is the third field the reach decides: only a landscape
            // is larger than one bank of fog.
            EXPECT_EQ(near.mUniform, 0.0f);

            Settings::rtx().mDistantLandCells.set(8.0f);
            const Fog far = exteriorFog(haze, 0.69f, 0.0f);
            EXPECT_EQ(far.mEdge, 8.0f * cell);
            EXPECT_NEAR(far.mExtinction, 0.5f * near.mExtinction, 1e-10f) << "twice the world, half the air";

            // **Clear weather in dead still air is the layer `FOG_HEIGHT` names**, which is what
            // makes every other weather a multiple of it rather than a number of its own.
            EXPECT_FLOAT_EQ(near.mLift, 1.0f);
            EXPECT_EQ(near.mWind, 0.0f);

            // The wind it was read with rides along, for the frame to point along the deck's bearing.
            EXPECT_EQ(exteriorFog(haze, 0.69f, 0.3f).mWind, 0.3f);

            // A room is none of that: a fixed reach, still air, and no ring of cut ground to close
            // over however much world stands outside its walls.
            const Fog room = roomFog(haze, 0.75f);
            EXPECT_EQ(room.mColour, haze);
            EXPECT_EQ(room.mEdge, 0.0f);
            EXPECT_EQ(room.mUniform, 1.0f);
            EXPECT_NEAR(room.mExtinction, fogExtinction(0.75f, sInteriorFogReach), 1e-10f);

            Settings::rtx().mDistantLandCells.set(4.0f);
            EXPECT_EQ(roomFog(haze, 0.75f).mExtinction, room.mExtinction) << "a cellar reading the sky's size";
        }
    }
}
