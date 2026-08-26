#include <gtest/gtest.h>

#include <components/esm3/loadcell.hpp>
#include <components/rtx/fogbuilder.hpp>
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
            ESM::Cell room;
            room.mHasAmbi = true;
            room.mAmbi.mFogDensity = 0.75f;
            room.mAmbi.mFog = 0x00808080;

            const float thick = interiorFog(room).mExtinction;
            EXPECT_NEAR(thick, 6.1888e-6f, 1e-10f) << "the customs office, from its own record alone";

            Settings::camera().mViewingDistance.set(4.0f * 8192.0f);
            EXPECT_FLOAT_EQ(interiorFog(room).mExtinction, thick) << "a cellar cleared because the sky got bigger";

            Settings::camera().mViewingDistance.set(2048.0f);
            EXPECT_FLOAT_EQ(interiorFog(room).mExtinction, thick) << "and thickened because it got smaller";

            Settings::camera().mViewingDistance.set(7168.0f);
        }
    }
}
