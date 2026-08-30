#include <cmath>
#include <cstddef>

#include <gtest/gtest.h>

#include <components/fallback/fallback.hpp>
#include <components/rtx/moonbuilder.hpp>
#include <components/rtx/shaders/colour.h>
#include <components/rtx/shaders/scene.h>

#include "allocations.hpp"

namespace Rtx
{
    namespace
    {
        /// Morrowind's own `[Moons]`, as the ini ships it.
        ///
        /// **Morrowind's own numbers, so this stands up with no installation present.**
        ///
        /// `Fallback::Map::init` keeps the first value it is given for a key, and a test elsewhere in
        /// this binary opens the real installation — so whichever ran first is what these tests see.
        /// Every value below is the ini's, and every one of them matches the default OpenMW ships
        /// **except the two sizes**, where the ini says 94 and 40 against OpenMW's 55 and 20. That is
        /// why nothing here asserts a size, and why everything that turns on the arc does: the
        /// speeds, the increments and the fade angles agree between the two.
        void seed()
        {
            Fallback::Map::init({
                { "Moons_Masser_Size", "94" },
                { "Moons_Masser_Fade_In_Start", "14" },
                { "Moons_Masser_Fade_In_Finish", "15" },
                { "Moons_Masser_Fade_Out_Start", "7" },
                { "Moons_Masser_Fade_Out_Finish", "10" },
                { "Moons_Masser_Axis_Offset", "35" },
                { "Moons_Masser_Speed", "0.5" },
                { "Moons_Masser_Daily_Increment", "1" },
                { "Moons_Masser_Fade_Start_Angle", "50" },
                { "Moons_Masser_Fade_End_Angle", "40" },
                { "Moons_Masser_Moon_Shadow_Early_Fade_Angle", "0.5" },

                { "Moons_Secunda_Size", "40" },
                { "Moons_Secunda_Fade_In_Start", "14" },
                { "Moons_Secunda_Fade_In_Finish", "15" },
                { "Moons_Secunda_Fade_Out_Start", "7" },
                { "Moons_Secunda_Fade_Out_Finish", "10" },
                { "Moons_Secunda_Axis_Offset", "50" },
                { "Moons_Secunda_Speed", "0.6" },
                { "Moons_Secunda_Daily_Increment", "1.2" },
                { "Moons_Secunda_Fade_Start_Angle", "50" },
                { "Moons_Secunda_Fade_End_Angle", "40" },
                { "Moons_Secunda_Moon_Shadow_Early_Fade_Angle", "0.5" },
            });
        }

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

        /// Degrees along its arc that Masser covers in an hour.
        ///
        /// **Not the 0.5 the ini asks for.** Fifteen degrees an hour is one rotation a day and the
        /// speed counts rotations, so 0.5 would leave the moon short of its own horizon in
        /// twenty-four hours; the engine floors every moon at `180 / 23 / 15` and Masser is the one
        /// that hits the floor. 15 * 0.5217391 is 7.826087 degrees an hour.
        constexpr float sMasserPerHour = 15.0f * (180.0f / 23.0f / 15.0f);

        /// A moon is as wide as the renderer the game already has draws it.
        ///
        /// `Moons_<name>_Size` is scaled by 450/125 onto a quad of half-extent 0.5 a thousand units
        /// off (`apps/openmw/mwrender/gl/skyutil.cpp:641`), so the radius is `atan(1.8 * size /
        /// 1000)` — the closed form this checks the code against.
        ///
        /// **The size itself is not pinned here, and deliberately.** Morrowind's own ini says 94 and
        /// 40, OpenMW ships defaults of 55 and 20, and which pair a run sees depends on the order
        /// the suite planted its fallback keys. The conversion is what this owns; the number is
        /// whatever the installation is configured with, and both give a moon far larger than the
        /// real one — 9.6 degrees of radius or 5.7, against the quarter of a degree ours has.
        TEST(RtxMoonBuilderTest, aMoonIsAsWideAsTheGameDrawsIt)
        {
            seed();

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

        /// Masser rises at four in the afternoon on the day the game begins, and climbs from there.
        ///
        /// The rise hour is `increment + (day - 1 + 16) * increment mod 24`, and Masser's increment
        /// is one, so day zero gives `1 + 15`. At the horizon the moon's height is nothing; six
        /// hours later it has travelled `6 * 7.826087` degrees and stands at the sine of that.
        ///
        /// **The height is the sine of the arc and the axis offset does not enter it**, which is the
        /// whole reason the offset swings the arc about the zenith rather than tipping it: both
        /// moons climb as high as the sun does and only their rising points differ.
        TEST(RtxMoonBuilderTest, masserRisesAtSixteenHundredOnTheDayTheGameBegins)
        {
            seed();

            const MoonPlacement rising = makeMoon(Moon::Masser, 0, 16.0f, 1.0f);
            EXPECT_NEAR(rising.mDirection.z(), 0.0f, 1e-6f) << "on the horizon at the moment it rises";

            const MoonPlacement up = makeMoon(Moon::Masser, 0, 22.0f, 1.0f);
            const float travelled = osg::DegreesToRadians(6.0f * sMasserPerHour);
            EXPECT_NEAR(up.mDirection.z(), std::sin(travelled), 1e-4f);

            // Due north swung 35 degrees, at the cosine of the arc: `(-cos a sin 35, cos a cos 35)`.
            EXPECT_NEAR(up.mDirection.x(), -std::cos(travelled) * std::sin(osg::DegreesToRadians(35.0f)), 1e-4f);
            EXPECT_NEAR(up.mDirection.y(), std::cos(travelled) * std::cos(osg::DegreesToRadians(35.0f)), 1e-4f);

            // Secunda's arc is swung further and runs faster, so the two rise apart and cross.
            const MoonPlacement other = makeMoon(Moon::Secunda, 0, 22.0f, 1.0f);
            EXPECT_GT(std::abs(other.mDirection.x() - up.mDirection.x()), 0.1f);
        }

        /// The face is a frame, not a billboard: three unit vectors at right angles to each other.
        TEST(RtxMoonBuilderTest, theFaceStandsSquareToWhereTheMoonIs)
        {
            seed();

            for (const float hour : { 17.0f, 20.0f, 23.0f })
            {
                const MoonPlacement at = makeMoon(Moon::Masser, 0, hour, 1.0f);
                EXPECT_NEAR(at.mDirection.length(), 1.0f, 1e-5f) << "at hour " << hour;
                EXPECT_NEAR(at.mRight.length(), 1.0f, 1e-5f) << "at hour " << hour;
                EXPECT_NEAR(at.mUp.length(), 1.0f, 1e-5f) << "at hour " << hour;

                EXPECT_NEAR(at.mRight * at.mUp, 0.0f, 1e-5f) << "at hour " << hour;
                EXPECT_NEAR(at.mRight * at.mDirection, 0.0f, 1e-5f) << "at hour " << hour;
                EXPECT_NEAR(at.mUp * at.mDirection, 0.0f, 1e-5f) << "at hour " << hour;
            }

            // **And it turns against the horizon as the moon crosses**, which is what a locked moon
            // does and what a billboard does not: the face's up is not the world's.
            const osg::Vec3f early = makeMoon(Moon::Masser, 0, 17.0f, 1.0f).mUp;
            const osg::Vec3f late = makeMoon(Moon::Masser, 0, 23.0f, 1.0f).mUp;
            EXPECT_LT(early * late, 0.99f) << "the portrait would be pinned to the horizon";
        }

        /// A moon arrives and leaves twice over: by the hour, and by whatever the weather lets
        /// through.
        ///
        /// **The third way the engine has is deliberately not here.** `MoonMoment::mAlpha` also hides
        /// a moon under `Fade_End_Angle`, and the ray tracer takes `mDaylightFade` instead — see
        /// `aMoonRisesOutOfTheHorizonRatherThanArrivingAboveIt`.
        TEST(RtxMoonBuilderTest, aMoonIsFadedByTheHourAndByTheWeather)
        {
            seed();

            // Day nine is where Masser rises at one in the morning — `1 + (9 - 1 + 16) mod 24` — so
            // half past two in the afternoon finds it a hundred and six degrees along and inside the
            // hour-long fade in that runs from fourteen to fifteen. Half an hour of one hour is half
            // the moon.
            EXPECT_FLOAT_EQ(makeMoon(Moon::Masser, 9, 14.5f, 1.0f).mAlpha, 0.5f);

            // And between the fade out finishing and the fade in starting there is no moon at all,
            // whatever its arc says.
            EXPECT_EQ(makeMoon(Moon::Masser, 9, 12.0f, 1.0f).mAlpha, 0.0f);

            // The weather has the last word on all of it, which is the `adjustTransparency` the
            // rasterizer calls with `Glare_View` after the moon's own state is settled.
            EXPECT_FLOAT_EQ(makeMoon(Moon::Masser, 0, 22.0f, 0.25f).mAlpha, 0.25f);
            EXPECT_EQ(makeMoon(Moon::Masser, 0, 22.0f, 0.0f).mIrradiance, osg::Vec3f()) << "a thunderstorm";
        }

        /// It rises out of the horizon, dimmed and reddened by the air rather than switched off.
        ///
        /// **What the engine does instead is draw no moon at all under `Fade_End_Angle`** — thirty
        /// degrees for Secunda, forty for Masser — because a lit quad over its own fogged dome reads
        /// as a sticker. Nothing here needs that: `Rtx::airTransmittance` takes a low moon out on the
        /// slant path, and takes the blue out first, so one comes over the edge as a deep red ember.
        ///
        /// **Masser rises at sixteen hundred on day zero** and climbs 7.826 degrees an hour, so
        /// seventeen hundred is eight degrees up — inside the arc the engine draws nothing over.
        TEST(RtxMoonBuilderTest, aMoonRisesOutOfTheHorizonRatherThanArrivingAboveIt)
        {
            seed();

            const MoonPlacement low = makeMoon(Moon::Masser, 0, 17.0f, 1.0f);
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

        /// The game begins under a full moon, and it wanes from there on a three-day cycle.
        ///
        /// `(day + 1) / 3 mod 8` counts the eight painted phases from full once the moon has risen,
        /// so days zero through one are full, two through four the first step off it, and the eighth
        /// step comes back round. Zero radians is full and pi is new, which puts new — the fifth of
        /// the eight — at days twelve to fourteen.
        TEST(RtxMoonBuilderTest, theGameBeginsFullAndWanesOnAThreeDayCycle)
        {
            seed();

            EXPECT_FLOAT_EQ(makeMoon(Moon::Masser, 0, 22.0f, 1.0f).mPhaseAngle, 0.0f) << "16 Last Seed";
            EXPECT_FLOAT_EQ(makeMoon(Moon::Masser, 1, 22.0f, 1.0f).mPhaseAngle, 0.0f);

            // A quarter turn of the cycle is one of the eight steps, and the steps go by threes.
            EXPECT_FLOAT_EQ(makeMoon(Moon::Masser, 2, 22.0f, 1.0f).mPhaseAngle, 0.25f * osg::PIf);
            EXPECT_FLOAT_EQ(makeMoon(Moon::Masser, 5, 22.0f, 1.0f).mPhaseAngle, 0.5f * osg::PIf);

            // Halfway round is new, twelve days in — a moon that is up and unlit.
            EXPECT_FLOAT_EQ(makeMoon(Moon::Masser, 11, 22.0f, 1.0f).mPhaseAngle, osg::PIf);

            // And a full cycle is twenty-four days, which is the loop the rise hour runs on too. The
            // count is of tomorrow rather than today, so the last of the eight steps is days twenty
            // to twenty-two and the twenty-third is already back at full.
            EXPECT_FLOAT_EQ(makeMoon(Moon::Masser, 21, 22.0f, 1.0f).mPhaseAngle, 1.75f * osg::PIf);
            EXPECT_FLOAT_EQ(makeMoon(Moon::Masser, 23, 22.0f, 1.0f).mPhaseAngle, 0.0f) << "back to full";

            // **The lit share is the cosine, and it is what the shader carves the terminator with.**
            // Full is all of it, the two quarters are half, and new is none.
            const auto lit = [](float phaseAngle) { return 0.5f * (1.0f + std::cos(phaseAngle)); };
            EXPECT_FLOAT_EQ(lit(makeMoon(Moon::Masser, 0, 22.0f, 1.0f).mPhaseAngle), 1.0f);
            EXPECT_NEAR(lit(makeMoon(Moon::Masser, 5, 22.0f, 1.0f).mPhaseAngle), 0.5f, 1e-6f);
            EXPECT_NEAR(lit(makeMoon(Moon::Masser, 11, 22.0f, 1.0f).mPhaseAngle), 0.0f, 1e-6f);
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
            seed();

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

            seed();
            const float masser = share(moonAngularRadius(Moon::Masser));
            EXPECT_LT(masser, 1.0f / 250.0f) << "a moon brighter than a sunrise";
            EXPECT_GT(masser, 1.0f / 1000.0f) << "a moon that lights nothing";
            EXPECT_GT(masser / share(osg::DegreesToRadians(0.2593f)), 400.0f) << "the size the game gives it got lost";
        }

        /// Secunda delivers the share of the sky it covers, with its own albedo on top.
        ///
        /// **What a moon is worth as a light goes as the sky it covers.** Both moons are the same
        /// law at the same albedo, so Secunda's share of Masser's is the ratio of their sines
        /// squared: 0.1853 at the ini's sizes. Its portrait is the paler of the two by 2.54, so what
        /// it actually delivers is 0.4705 of Masser rather than 0.1853.
        TEST(RtxMoonBuilderTest, secundaDeliversTheShareOfTheSkyItCovers)
        {
            seed();

            const MoonPlacement masser = placeMoon(Moon::Masser, 90.0f, 35.0f, /*phase=*/0, /*alpha=*/1.0f);
            const MoonPlacement secunda = placeMoon(Moon::Secunda, 90.0f, -50.0f, /*phase=*/0, /*alpha=*/1.0f);

            const float wide = std::sin(moonAngularRadius(Moon::Masser));
            const float narrow = std::sin(moonAngularRadius(Moon::Secunda));
            EXPECT_NEAR((narrow * narrow) / (wide * wide), 0.1853f, 1e-4f);

            EXPECT_NEAR(luminanceOf(sentBy(secunda)) / luminanceOf(sentBy(masser)), 0.4705f, 1e-3f);

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
            seed();

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
            seed();

            EXPECT_EQ(placeMoon(Moon::Masser, 90.0f, 35.0f, /*phase=*/0, /*alpha=*/0.0f).mIrradiance, osg::Vec3f());

            // The fade is a plain multiplier on it, so half hidden is half lit.
            const MoonPlacement full = placeMoon(Moon::Masser, 90.0f, 35.0f, /*phase=*/0, /*alpha=*/1.0f);
            const MoonPlacement half = placeMoon(Moon::Masser, 90.0f, 35.0f, /*phase=*/0, /*alpha=*/0.5f);
            EXPECT_NEAR(luminanceOf(half.mIrradiance), 0.5f * luminanceOf(full.mIrradiance), 1e-7f);
        }

        /// Placing a moon goes to the heap not at all, and answers the same either way.
        ///
        /// **Both moons are placed on every frame, by the game and by the harness alike.** A clock
        /// is ten `Moons_*` lookups and a size is one more, every one of them a key built on the
        /// spot — twenty-two allocations a frame, for numbers that are fixed for the run.
        TEST(RtxMoonBuilderTest, placingAMoonReadsNothingItHasAlreadyRead)
        {
            seed();

            const MoonPlacement first = makeMoon(Moon::Masser, 3, 21.0f, 1.0f);

            const std::size_t before = Testing::getAllocationCount();
            const MoonPlacement again = makeMoon(Moon::Masser, 3, 21.0f, 1.0f);
            const std::size_t after = Testing::getAllocationCount();

            EXPECT_EQ(after, before) << after - before << " allocations to place a moon";

            EXPECT_EQ(again.mDirection, first.mDirection);
            EXPECT_EQ(again.mAlpha, first.mAlpha);
            EXPECT_EQ(again.mAngularRadius, first.mAngularRadius);
        }
    }
}
