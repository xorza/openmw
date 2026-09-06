#include <cmath>
#include <cstddef>

#include <gtest/gtest.h>

#include <components/fallback/fallback.hpp>
#include <components/rtx/moonbuilder.hpp>
#include <components/rtx/shaders/colour.h>
#include <components/rtx/shaders/look.h>
#include <components/rtx/shaders/scene.h>

#include "allocations.hpp"

namespace Rtx
{
    namespace
    {
        /// How this renderer weighs a colour into a brightness, which is what a level is measured in.
        float luminanceOf(const osg::Vec3f& linear)
        {
            return linear * Shaders::LUMINANCE_WEIGHTS;
        }

        /// What a moon sent, with the air it was seen through taken back off.
        ///
        /// **Two facts and two tests.** How much light a moon of that size and albedo delivers is one
        /// question, and what the air between it and the eye leaves of that is another — so the
        /// tests about the first divide the second out rather than carrying it in their numbers.
        osg::Vec3f sentBy(const MoonPlacement& moon)
        {
            return osg::componentDivide(moon.mIrradiance, moon.mThroughAir);
        }

        /// A moon is as wide as the renderer the game already has draws it.
        ///
        /// `Moons_<name>_Size` is scaled by 450/125 onto a quad of half-extent 0.5 a thousand units
        /// off (`apps/openmw/mwrender/gl/skyutil.cpp:641`), so the radius is `atan(1.8 * size /
        /// 1000)` — the closed form this checks the code against.
        ///
        /// **The size itself is not pinned here, and deliberately.** Morrowind's own ini says 94 and
        /// 40 and OpenMW ships defaults of 55 and 20. The conversion is what this owns; the number is
        /// whatever the run is configured with, and both give a moon far larger than the real one —
        /// 9.6 degrees of radius or 5.7, against the quarter of a degree ours has.
        TEST(RtxMoonBuilderTest, aMoonIsAsWideAsTheGameDrawsIt)
        {
            const float masser = Fallback::Map::getFloat("Moons_Masser_Size");
            const float secunda = Fallback::Map::getFloat("Moons_Secunda_Size");

            EXPECT_NEAR(moonAngularRadius(Moon::Masser), std::atan(1.8f * masser / 1000.0f), 1e-6f);
            EXPECT_NEAR(moonAngularRadius(Moon::Secunda), std::atan(1.8f * secunda / 1000.0f), 1e-6f);

            // **Enormous either way**, which is the sky Morrowind is remembered for: the smaller of
            // the two pairs still puts Masser at twelve times the real moon's quarter degree.
            EXPECT_GT(osg::RadiansToDegrees(moonAngularRadius(Moon::Masser)), 3.0f);

            // Masser is the larger, and the angles are closer together than the sizes are: the
            // arctangent is already bending at a disc this wide.
            EXPECT_GT(moonAngularRadius(Moon::Masser), moonAngularRadius(Moon::Secunda));
            EXPECT_LT(moonAngularRadius(Moon::Masser) / moonAngularRadius(Moon::Secunda), masser / secunda);
        }

        /// The direction is the arc tipped up from the horizon and swung about the zenith.
        ///
        /// **Swung and not tipped**, which is what makes both moons climb as high as the sun does
        /// and only their rising points differ — so their paths cross. `Sky::MoonModel` is what
        /// says which angles an hour comes to; this is what a moon *is* once they are known.
        TEST(RtxMoonBuilderTest, aMoonStandsWhereItsArcAndItsOffsetPutIt)
        {
            const MoonPlacement risen = placeMoon(Moon::Masser, 0.0f, 35.0f, 0, 1.0f);
            EXPECT_NEAR(risen.mDirection.z(), 0.0f, 1e-6f) << "no height at the horizon it rises from";

            constexpr float sAlong = 47.0f;
            constexpr float sOffset = 35.0f;
            const MoonPlacement up = placeMoon(Moon::Masser, sAlong, sOffset, 0, 1.0f);

            const float along = osg::DegreesToRadians(sAlong);
            const float swung = osg::DegreesToRadians(sOffset);

            // Due north swung about the zenith, at the cosine of the arc: `(-cos a sin o, cos a cos
            // o, sin a)`. The height is the sine of the arc and the offset does not enter it.
            EXPECT_NEAR(up.mDirection.z(), std::sin(along), 1e-5f);
            EXPECT_NEAR(up.mDirection.x(), -std::cos(along) * std::sin(swung), 1e-5f);
            EXPECT_NEAR(up.mDirection.y(), std::cos(along) * std::cos(swung), 1e-5f);

            // A wider swing puts the same arc somewhere else, which is what separates the two moons.
            const MoonPlacement other = placeMoon(Moon::Secunda, sAlong, 50.0f, 0, 1.0f);
            EXPECT_GT(std::abs(other.mDirection.x() - up.mDirection.x()), 0.1f);
        }

        /// The face is a frame, not a billboard: three unit vectors at right angles to each other.
        TEST(RtxMoonBuilderTest, theFaceStandsSquareToWhereTheMoonIs)
        {
            for (const float along : { 8.0f, 47.0f, 94.0f })
            {
                const MoonPlacement at = placeMoon(Moon::Masser, along, 35.0f, 0, 1.0f);
                EXPECT_NEAR(at.mDirection.length(), 1.0f, 1e-5f) << "along " << along;
                EXPECT_NEAR(at.mRight.length(), 1.0f, 1e-5f) << "along " << along;
                EXPECT_NEAR(at.mUp.length(), 1.0f, 1e-5f) << "along " << along;

                EXPECT_NEAR(at.mRight * at.mUp, 0.0f, 1e-5f) << "along " << along;
                EXPECT_NEAR(at.mRight * at.mDirection, 0.0f, 1e-5f) << "along " << along;
                EXPECT_NEAR(at.mUp * at.mDirection, 0.0f, 1e-5f) << "along " << along;
            }

            // **And it turns against the horizon as the moon crosses**, which is what a locked moon
            // does and what a billboard does not: the face's up is not the world's.
            const osg::Vec3f early = placeMoon(Moon::Masser, 8.0f, 35.0f, 0, 1.0f).mUp;
            const osg::Vec3f late = placeMoon(Moon::Masser, 94.0f, 35.0f, 0, 1.0f).mUp;
            EXPECT_LT(early * late, 0.99f) << "the portrait would be pinned to the horizon";
        }

        /// A moon that is not on its arc is not drawn, and the weather has the last word on one that
        /// is.
        ///
        /// **Two of the three ways a moon goes out.** The hour's own fade is
        /// `Sky::MoonMoment::mDaylightFade` and is asserted where the clock is; what reaches here is
        /// that number with `Glare_View` already on it, which is the `adjustTransparency` the
        /// rasterizer applies after the moon's state is settled.
        TEST(RtxMoonBuilderTest, aMoonOffItsArcIsNotDrawnAndTheWeatherDimsOneThatIs)
        {
            // **Nought until it is on its arc**, which the engine states by leaving the angle there
            // until a moon rises and returning it there once it sets.
            EXPECT_EQ(placeMoon(Moon::Masser, 0.0f, 35.0f, 0, 1.0f).mAlpha, 0.0f);

            EXPECT_FLOAT_EQ(placeMoon(Moon::Masser, 47.0f, 35.0f, 0, 0.5f).mAlpha, 0.5f);
            EXPECT_FLOAT_EQ(placeMoon(Moon::Masser, 47.0f, 35.0f, 0, 0.25f).mAlpha, 0.25f);
            EXPECT_EQ(placeMoon(Moon::Masser, 47.0f, 35.0f, 0, 0.0f).mIrradiance, osg::Vec3f()) << "a thunderstorm";
        }

        /// It rises out of the horizon, dimmed and reddened by the air rather than switched off.
        ///
        /// **What the engine does instead is draw no moon at all under `Fade_End_Angle`** — thirty
        /// degrees for Secunda, forty for Masser — because a lit quad over its own fogged dome reads
        /// as a sticker. Nothing here needs that: `Rtx::airTransmittance` takes a low moon out on the
        /// slant path, and takes the blue out first, so one comes over the edge as a deep red ember.
        ///
        /// **Eight degrees up is inside the arc the engine draws nothing over** — an hour after
        /// Masser rises, by `Sky::MoonModel`'s clock.
        TEST(RtxMoonBuilderTest, aMoonRisesOutOfTheHorizonRatherThanArrivingAboveIt)
        {
            const MoonPlacement low = placeMoon(Moon::Masser, 7.826f, 35.0f, 0, 1.0f);
            EXPECT_NEAR(osg::RadiansToDegrees(std::asin(low.mDirection.z())), 7.826f, 0.01f);

            EXPECT_FLOAT_EQ(low.mAlpha, 1.0f) << "the engine's own arc gate is still in the way";
            EXPECT_GT(luminanceOf(low.mIrradiance), 0.0f) << "and it lights nothing down there";

            // Reddened, not merely dimmed: eight degrees is 6.6 air masses, which leaves a fifth of
            // the blue against two thirds of the red.
            EXPECT_LT(low.mThroughAir.z(), 0.5f * low.mThroughAir.y());
            EXPECT_LT(low.mThroughAir.y(), low.mThroughAir.x());

            // And it keeps climbing into itself, with no step anywhere along the way.
            float below = 0.0f;
            for (int step = 1; step <= 90; ++step)
            {
                const MoonPlacement at = placeMoon(Moon::Masser, float(step), 35.0f, /*phase=*/0, /*alpha=*/1.0f);
                const float carried = luminanceOf(at.mThroughAir);

                EXPECT_GT(carried, below) << "at " << step << " degrees along";
                below = carried;
            }

            // A moon that is not on its arc is not in the sky, which the engine says by leaving the
            // angle at nought both before it rises and after it sets.
            const MoonPlacement down = placeMoon(Moon::Masser, 0.0f, 35.0f, /*phase=*/0, /*alpha=*/1.0f);
            EXPECT_EQ(down.mAlpha, 0.0f);
            EXPECT_EQ(down.mIrradiance, osg::Vec3f());
        }

        /// A painted phase is an angle: zero at full, pi at new, and an eighth of a turn a step.
        ///
        /// **The steps are even, which is what lets one index stand for an angle.** `Sky::MoonPhase`
        /// declares the eight in the game's own order and `SkyMoonTest` is where the clock's walk
        /// over them is asserted; what is here is the angle each of them becomes.
        TEST(RtxMoonBuilderTest, aPaintedPhaseIsAnAngleFromFull)
        {
            const auto angleOf
                = [](const int phase) { return placeMoon(Moon::Masser, 47.0f, 35.0f, phase, 1.0f).mPhaseAngle; };

            EXPECT_FLOAT_EQ(angleOf(0), 0.0f) << "full";
            EXPECT_FLOAT_EQ(angleOf(1), 0.25f * osg::PIf);
            EXPECT_FLOAT_EQ(angleOf(2), 0.5f * osg::PIf);
            EXPECT_FLOAT_EQ(angleOf(4), osg::PIf) << "new, halfway round";
            EXPECT_FLOAT_EQ(angleOf(7), 1.75f * osg::PIf);

            // **The lit share is the cosine, and it is what the shader carves the terminator with.**
            // Full is all of it, the two quarters are half, and new is none.
            const auto lit = [](float phaseAngle) { return 0.5f * (1.0f + std::cos(phaseAngle)); };
            EXPECT_FLOAT_EQ(lit(angleOf(0)), 1.0f);
            EXPECT_NEAR(lit(angleOf(2)), 0.5f, 1e-6f);
            EXPECT_NEAR(lit(angleOf(4)), 0.0f, 1e-6f);
        }

        /// A full Masser delivers what a lit disc of its size and albedo delivers, and no more.
        ///
        /// **Reached the long way round here and the short way in the code.** A moon is a Lambertian
        /// body under the sun, so its radiance at opposition is `E * p / pi`; a disc of that radiance
        /// and half-angle `t` delivers `L * pi * sin(t)^2` to whatever faces it. The two `pi` cancel
        /// and `placeMoon` writes what is left, so building it back up from the radiance is a second
        /// path to the same number.
        ///
        /// **The level is Masser's, and it stays Masser's because the two tints are normalised on
        /// Masser's own luminance.** The portraits differ in brightness as well as in hue, and that
        /// difference is a fact about the bodies rather than the art — so a single albedo can only
        /// speak for one moon, and this is the one it speaks for.
        TEST(RtxMoonBuilderTest, aFullMasserDeliversWhatALitDiscOfItsSizeDoes)
        {
            const float radiance = Shaders::DAYLIGHT * Shaders::MOON_ALBEDO * Shaders::INV_PI;
            const float sine = std::sin(moonAngularRadius(Moon::Masser));
            const float facing = radiance * osg::PIf * sine * sine;

            const MoonPlacement full = placeMoon(Moon::Masser, 90.0f, 35.0f, /*phase=*/0, /*alpha=*/1.0f);
            EXPECT_NEAR(luminanceOf(sentBy(full)), facing, 1e-6f);

            // And it is red, which is the only reason to draw Masser rather than a bright dot: its
            // portrait averages 0.0332 against 0.0099, and the light it reflects carries that.
            EXPECT_GT(full.mIrradiance.x(), 3.0f * full.mIrradiance.y());
        }

        /// The same formula, asked about the moon everyone can check.
        ///
        /// **A real full moon is a 407,000th of the sun**, which is the published figure and the one
        /// thing here that no content file can move. Half a degree of diameter and a geometric albedo
        /// of 0.12 give `0.12 * sin(0.2593 deg)^2 = 2.45e-6`, and that is the whole of what makes
        /// Morrowind's moons able to light anything at all: Masser is twenty-two to thirty-eight
        /// times wider, so it covers hundreds of times the sky.
        ///
        /// **A band and not a number, for the reason the sizes are not pinned above.** OpenMW's 55
        /// puts Masser at an 858th of the sun and the ini's 94 at a 299th, and which of the two a run
        /// sees is whichever planted the key first. Both are a night that can be lit and neither is a
        /// second sunrise, which is what the band asserts.
        TEST(RtxMoonBuilderTest, theSameLawPutsARealMoonWhereThePhotometryDoes)
        {
            const auto share = [](float angularRadius) {
                const float sine = std::sin(angularRadius);
                return Shaders::MOON_ALBEDO * sine * sine;
            };

            EXPECT_NEAR(share(osg::DegreesToRadians(0.2593f)), 1.0f / 407000.0f, 1e-8f);

            const float masser = share(moonAngularRadius(Moon::Masser));
            EXPECT_LT(masser, 1.0f / 250.0f) << "a moon brighter than a sunrise";
            EXPECT_GT(masser, 1.0f / 1000.0f) << "a moon that lights nothing";
            EXPECT_GT(masser / share(osg::DegreesToRadians(0.2593f)), 400.0f) << "the size the game gives it got lost";
        }

        /// Secunda delivers the share of the sky it covers, with its own albedo on top.
        ///
        /// **What a moon is worth as a light goes as the sky it covers.** Both moons are the same
        /// law at the same albedo, so Secunda's share of Masser's is the ratio of their sines
        /// squared, and its portrait being the paler of the two multiplies that back up.
        ///
        /// **The paleness is what this code owns and the sizes are not.** How much sky each covers
        /// comes out of whichever `Moons_*_Size` the run is configured with, while the ratio between
        /// the two portraits is a number measured off the shipped textures. At the ini's 94 and 40
        /// the share is 0.1853 and what Secunda delivers is 0.4705 of Masser; at OpenMW's 55 and 20
        /// both are something else and the ratio between them is the same 2.54.
        TEST(RtxMoonBuilderTest, secundaDeliversTheShareOfTheSkyItCovers)
        {
            const MoonPlacement masser = placeMoon(Moon::Masser, 90.0f, 35.0f, /*phase=*/0, /*alpha=*/1.0f);
            const MoonPlacement secunda = placeMoon(Moon::Secunda, 90.0f, -50.0f, /*phase=*/0, /*alpha=*/1.0f);

            const float wide = std::sin(moonAngularRadius(Moon::Masser));
            const float narrow = std::sin(moonAngularRadius(Moon::Secunda));
            const float covered = (narrow * narrow) / (wide * wide);
            EXPECT_LT(covered, 1.0f) << "Secunda is the smaller of the two, whatever the sizes say";

            const float delivered = luminanceOf(sentBy(secunda)) / luminanceOf(sentBy(masser));
            EXPECT_NEAR(delivered / covered, 2.54f, 1e-2f) << "the paler portrait, over the sky it covers";

            // The pale light of the two, where Masser's is red.
            EXPECT_LT(secunda.mIrradiance.x(), 1.4f * secunda.mIrradiance.y());
        }

        /// A quarter moon lights a tenth of what a full one does, and a new one lights nothing.
        ///
        /// **The measured law and not the lit fraction of the disc, which differ by a factor of
        /// five.** Half a disc lit would say half the light; photometry says 0.09, because the
        /// surface is rough enough to shadow itself everywhere but at opposition. Allen's fit
        /// `dm = 0.026|a| + 4e-9 a^4` is what gives that, and at 90 degrees it comes to 2.6024
        /// magnitudes, which is `10^-1.041` of full.
        TEST(RtxMoonBuilderTest, aQuarterMoonLightsATenthOfWhatAFullOneDoes)
        {
            const auto lightAt = [](int phase) {
                return luminanceOf(placeMoon(Moon::Masser, 90.0f, 35.0f, phase, /*alpha=*/1.0f).mIrradiance);
            };

            EXPECT_NEAR(lightAt(2) / lightAt(0), 0.090997f, 1e-5f);
            EXPECT_LT(lightAt(2) / lightAt(0), 0.2f) << "the lit fraction of the disc, rather than the photometry";

            // 180 degrees comes to 8.879 magnitudes, which is three parts in ten thousand.
            EXPECT_LT(lightAt(4) / lightAt(0), 0.001f);

            // Waxing and waning quarters deliver the same. Which limb keeps the light is the disc's
            // business, and how far from full the moon is is the light's.
            EXPECT_FLOAT_EQ(lightAt(6), lightAt(2));
        }

        /// A moon the game has faded out lights nothing at all.
        ///
        /// **Which is what keeps a daylit frame from spending a shadow ray on each of them.** The
        /// game fades both moons over the hours around dawn, and what a shader reads to decide
        /// whether a moon is worth a ray is this being nothing.
        TEST(RtxMoonBuilderTest, aFadedMoonLightsNothing)
        {
            EXPECT_EQ(placeMoon(Moon::Masser, 90.0f, 35.0f, /*phase=*/0, /*alpha=*/0.0f).mIrradiance, osg::Vec3f());

            // The fade is a plain multiplier on it, so half hidden is half lit.
            const MoonPlacement full = placeMoon(Moon::Masser, 90.0f, 35.0f, /*phase=*/0, /*alpha=*/1.0f);
            const MoonPlacement half = placeMoon(Moon::Masser, 90.0f, 35.0f, /*phase=*/0, /*alpha=*/0.5f);
            EXPECT_NEAR(luminanceOf(half.mIrradiance), 0.5f * luminanceOf(full.mIrradiance), 1e-7f);
        }

        /// Placing a moon goes to the heap not at all, and answers the same either way.
        ///
        /// **Both moons are placed on every frame.** A size is a `Moons_*` lookup and the key is
        /// built on the spot, so a placement that read one would allocate twice a frame for a
        /// number that is fixed for the run.
        TEST(RtxMoonBuilderTest, placingAMoonReadsNothingItHasAlreadyRead)
        {
            const MoonPlacement first = placeMoon(Moon::Masser, 47.0f, 35.0f, 3, 1.0f);

            const std::size_t before = Testing::getAllocationCount();
            const MoonPlacement again = placeMoon(Moon::Masser, 47.0f, 35.0f, 3, 1.0f);
            const std::size_t after = Testing::getAllocationCount();

            EXPECT_EQ(after, before) << after - before << " allocations to place a moon";

            EXPECT_EQ(again.mDirection, first.mDirection);
            EXPECT_EQ(again.mAlpha, first.mAlpha);
            EXPECT_EQ(again.mAngularRadius, first.mAngularRadius);
        }
    }
}
