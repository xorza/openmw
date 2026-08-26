#include <gtest/gtest.h>

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

        /// A room's air is thinner than the conversion above makes it, and by one shared number.
        ///
        /// **The ramp and the medium part company indoors** — `interiorFogReach` says at length why,
        /// and the short of it is that the ramp is clear across the whole of a room where a medium
        /// cannot be, and that a lit medium is not a colour a pixel is mixed toward. What is checked
        /// here is that the stretch is applied and that it is the *only* thing separating a room's
        /// extinction from the raw conversion, since the game reaches the same number by a different
        /// route and the two may not drift.
        TEST(RtxFogTest, aRoomsAirIsStretchedPastTheRangeItsRecordWasWrittenAgainst)
        {
            constexpr float view = 7168.0f;

            // Ten times the range is a tenth of the extinction: an exponential's half-life is
            // exactly the distance it is measured over.
            EXPECT_FLOAT_EQ(interiorFogReach(view), 10.0f * view);
            EXPECT_NEAR(fogExtinction(0.69f, interiorFogReach(view)) / fogExtinction(0.69f, view), 0.1f, 1e-6f)
                << "a room is thinner than the ramp it came from, by the stretch and by nothing else";

            // The Seyda Neen customs office, whose depth of 0.75 is what the numbers in
            // `interiorFogReach` were measured against:
            //
            //   sigma = ln(2) / (10 * 7168 * (1 - 0.375)) = 1.5472e-5 per unit
            //
            // against the 1.5472e-4 the unstretched conversion gives, which is what put a tenth of a
            // lamp-lit medium between the eye and a wall seven hundred units away.
            EXPECT_NEAR(fogExtinction(0.75f, interiorFogReach(view)), 1.5472e-5f, 1e-9f);

            // **Proportional and not a floor**, so a denser room is still the denser one: foggy's
            // 1.0 against clear's 0.69 keeps exactly the ratio it had before the stretch.
            EXPECT_NEAR(fogExtinction(1.0f, interiorFogReach(view)) / fogExtinction(0.69f, interiorFogReach(view)),
                fogExtinction(1.0f, view) / fogExtinction(0.69f, view), 1e-5f);

            // And a room the record gives no fog at all still has none, however far it is measured
            // over — the stretch scales an extinction and never creates one.
            EXPECT_EQ(fogExtinction(0.0f, interiorFogReach(view)), 0.0f);
        }
    }
}
