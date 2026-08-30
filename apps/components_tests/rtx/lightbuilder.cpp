#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <components/esm3/loadcell.hpp>
#include <components/esm3/loadligh.hpp>
#include <components/esm3/loadregn.hpp>
#include <components/fallback/fallback.hpp>
#include <components/rtx/distantland.hpp>
#include <components/rtx/error.hpp>
#include <components/rtx/lightbuilder.hpp>

#include <components/rtx/shaders/visibility.h>
#include <components/sceneutil/lightcommon.hpp>
#include <components/sceneutil/lightcontroller.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/lightutil.hpp>
#include <components/sceneutil/util.hpp>
#include <components/sceneutil/vismask.hpp>
#include <components/sky/sun.hpp>
#include <components/weather/downpour.hpp>

namespace Rtx
{
    namespace
    {
        ESM::Light makeRecord(std::int32_t radius, std::uint32_t colour, std::int32_t flags)
        {
            ESM::Light record;
            record.mData.mRadius = radius;
            record.mData.mColor = colour;
            record.mData.mFlags = flags;
            return record;
        }

        osg::ref_ptr<SceneUtil::LightSource> makeGraphLight(const osg::Vec4f& diffuse, const osg::Vec4f& ambient)
        {
            osg::ref_ptr<SceneUtil::Light> light = new SceneUtil::Light;
            light->setDiffuse(diffuse);
            light->setAmbient(ambient);

            osg::ref_ptr<SceneUtil::LightSource> source = new SceneUtil::LightSource;
            source->setLight(light);
            return source;
        }

        /// One light's animation, sampled.
        struct Lamp
        {
            SceneUtil::LightController::LightType mType;
            int mId = 1;

            float at(double seconds) const { return lightBrightness(mType, mId, seconds); }

            /// `count` samples, `step` seconds apart, from zero.
            std::vector<float> run(std::size_t count, double step) const
            {
                std::vector<float> out;
                out.reserve(count);

                for (std::size_t i = 0; i < count; ++i)
                    out.push_back(at(static_cast<double>(i) * step));

                return out;
            }
        };

        /// How often the light crosses its own resting brightness, per second.
        float crossingsPerSecond(const std::vector<float>& run, double step)
        {
            std::size_t crossings = 0;
            for (std::size_t i = 1; i < run.size(); ++i)
                if ((run[i] - 1.0f) * (run[i - 1] - 1.0f) < 0.0f)
                    ++crossings;

            return static_cast<float>(static_cast<double>(crossings) / (static_cast<double>(run.size() - 1) * step));
        }

        float mean(const std::vector<float>& run)
        {
            double total = 0.0;
            for (const float value : run)
                total += static_cast<double>(value);

            return static_cast<float>(total / static_cast<double>(run.size()));
        }

        /// A light the record says nothing about burns at exactly what it is.
        TEST(RtxLightBuilderTest, aSteadyLightIsExactlyOne)
        {
            const Lamp steady{ SceneUtil::LightController::LT_Normal };

            for (const double seconds : { 0.0, 0.017, 3.5, 1e5 })
                EXPECT_EQ(steady.at(seconds), 1.0f);
        }

        /// A flame stays inside its depth and, over time, radiates exactly what the record says.
        ///
        /// **The mean is the point.** The rasterizer's own animation walks toward a random target
        /// between a quarter and one, so a flickering light averages 0.63 of its recorded colour;
        /// this one averages the colour itself, so a candle is as bright as the record says it is.
        TEST(RtxLightBuilderTest, aFlameStaysWithinItsDepthAndAveragesOne)
        {
            for (const SceneUtil::LightController::LightType type :
                { SceneUtil::LightController::LT_Flicker, SceneUtil::LightController::LT_FlickerSlow,
                    SceneUtil::LightController::LT_Pulse, SceneUtil::LightController::LT_PulseSlow })
            {
                const Lamp lamp{ type };
                const std::vector<float> run = lamp.run(60000, 0.01);
                const bool pulse
                    = type == SceneUtil::LightController::LT_Pulse || type == SceneUtil::LightController::LT_PulseSlow;

                // The bands are weighted to sum to one, so the depth is a bound and not a statistic.
                const float depth = pulse ? 0.35f : 0.30f;
                EXPECT_GE(*std::min_element(run.begin(), run.end()), 1.0f - depth);
                EXPECT_LE(*std::max_element(run.begin(), run.end()), 1.0f + depth);

                // Ten minutes is at least a hundred turns of the slowest band any of them carries,
                // so what is left of it here is a thousandth.
                EXPECT_NEAR(mean(run), 1.0f, 0.001f);

                // And it did move, rather than sitting at its mean and passing the two tests above.
                EXPECT_GT(*std::max_element(run.begin(), run.end()) - *std::min_element(run.begin(), run.end()), depth);
            }
        }

        /// The fast flicker is the flame itself and the slow one is that flame seen through glass.
        ///
        /// Both are four bands of one ladder; the slow one takes its window a step down, so it loses
        /// the puffing at the top and gains a drift at the bottom. One step of the ladder is 2.618,
        /// and the rate at which the light crosses its own mean follows it: about 11 times a second
        /// against about 4.
        TEST(RtxLightBuilderTest, theSlowFlickerIsTheSameFlameOneStepDownTheLadder)
        {
            const Lamp fast{ SceneUtil::LightController::LT_Flicker };
            const Lamp slow{ SceneUtil::LightController::LT_FlickerSlow };

            // 200 hertz, so the nine-hertz band's own crossings are resolved rather than counted
            // twice.
            const float busy = crossingsPerSecond(fast.run(12000, 0.005), 0.005);
            const float gentle = crossingsPerSecond(slow.run(12000, 0.005), 0.005);

            EXPECT_GT(busy, 8.0f);
            EXPECT_LT(gentle, 6.0f);
            EXPECT_GT(busy, gentle * 2.0f) << "the two flicker flags read as the same light";
        }

        /// A pulse is one sine, so it comes back to where it was and its two halves cancel exactly.
        ///
        /// The slow one turns once every three seconds and the fast one is a step of the ladder
        /// above it, at 3 / 2.618 = 1.1459 seconds.
        TEST(RtxLightBuilderTest, aPulseIsExactlyPeriodic)
        {
            const Lamp slow{ SceneUtil::LightController::LT_PulseSlow };

            for (const double seconds : { 0.0, 0.3, 1.7, 10.5, 123.25 })
            {
                EXPECT_NEAR(slow.at(seconds), slow.at(seconds + 3.0), 1e-5f);

                // Half a turn on, the sine is its own negative, so the pair averages the resting
                // brightness whatever phase this lamp was given.
                EXPECT_NEAR(slow.at(seconds) + slow.at(seconds + 1.5), 2.0f, 1e-5f);
            }

            const Lamp fast{ SceneUtil::LightController::LT_Pulse };
            constexpr double period = 3.0 / 2.618034;

            for (const double seconds : { 0.0, 0.3, 1.7, 10.5 })
                EXPECT_NEAR(fast.at(seconds), fast.at(seconds + period), 1e-5f);
        }

        /// The clock and the light's id are the whole of the state, so one instant is one answer.
        ///
        /// **What this buys is that anyone may ask.** This renderer's walk, the harness and a test
        /// all reach the same answer for a frame, at any frame rate, in any order, and however many
        /// times — which is what lets the light be computed where it is read rather than written
        /// once by whichever traversal got there first, of which the harness runs none.
        TEST(RtxLightBuilderTest, theSameInstantAlwaysGivesTheSameBrightness)
        {
            const Lamp lamp{ SceneUtil::LightController::LT_FlickerSlow };
            const std::vector<double> scrambled = { 4.5, 0.25, 91.0, 4.5, 0.25, 17.75, 91.0 };

            std::vector<float> first;
            for (const double seconds : scrambled)
                first.push_back(lamp.at(seconds));

            for (std::size_t i = 0; i < scrambled.size(); ++i)
                EXPECT_EQ(lamp.at(scrambled[i]), first[i]) << "at " << scrambled[i];

            EXPECT_EQ(first[0], first[3]);
            EXPECT_EQ(first[2], first[6]);
        }

        /// Two candles standing together do not flicker together.
        ///
        /// Their ids are the only thing separating them, and ids are handed out in sequence — so
        /// neighbours are exactly the case this has to answer for. Measured across sixty-four of
        /// them rather than between two.
        TEST(RtxLightBuilderTest, lampsBuiltTogetherStillFlickerApart)
        {
            std::vector<float> lit;
            for (int id = 0; id < 64; ++id)
                lit.push_back(Lamp{ SceneUtil::LightController::LT_PulseSlow, id }.at(0.0));

            double total = 0.0;
            for (const float value : lit)
                total += static_cast<double>(value);

            const double average = total / static_cast<double>(lit.size());
            double spread = 0.0;
            for (const float value : lit)
                spread += (static_cast<double>(value) - average) * (static_cast<double>(value) - average);

            // A pulse read at one instant across uniform phases has a deviation of 0.35 / sqrt(2),
            // which is 0.247. Half of that is far below anything sixty-four ids reach by chance and
            // far above the nothing a shared phase would give.
            EXPECT_GT(std::sqrt(spread / static_cast<double>(lit.size())), 0.12);
        }

        /// A light is dimmed by its owner, whatever it radiates with and whatever it is doing.
        ///
        /// **The fade reaches the ambient, which no animation does.** A Light spell's glow puts its
        /// whole output in the ambient, so a fade that reached only the diffuse would leave the glow
        /// burning at full strength up to the frame the actor's node mask cut it.
        TEST(RtxLightBuilderTest, aLightIsDimmedByItsOwnersFade)
        {
            const osg::Vec4f grey(128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f, 1.0f);
            const osg::Vec4f glow(1.5f, 1.5f, 1.5f, 1.0f);

            const osg::ref_ptr<SceneUtil::LightSource> full = makeGraphLight(grey, glow);
            const osg::ref_ptr<SceneUtil::LightSource> half = makeGraphLight(grey, glow);
            half->setActorFade(0.5f);

            EXPECT_NEAR(lightColour(*half, 0.0).x(), lightColour(*full, 0.0).x() * 0.5f, 1e-5f);

            // What the distance fade reaches exactly at `actors processing range`, which is the
            // frame before the node mask takes the whole actor out of the picture.
            const osg::ref_ptr<SceneUtil::LightSource> gone = makeGraphLight(grey, glow);
            gone->setActorFade(0.0f);

            EXPECT_EQ(lightColour(*gone, 0.0), osg::Vec3f());
        }

        /// The animation reaches the diffuse and stops there.
        ///
        /// **Because the ambient is not a flame.** The white one `ActorAnimation::addHiddenItemLight`
        /// adds is what a lamp in a pack lights its bearer with, and it has no flame of its own to
        /// flicker: a lantern the actor is not holding would otherwise pulse against a body it is
        /// nowhere near.
        TEST(RtxLightBuilderTest, anAnimationReachesTheDiffuseAndNotTheAmbient)
        {
            // A record with the slow pulse flag, so the light is built the way the game builds one:
            // a controller carrying the record's colour, added behind the collect callback.
            ESM::Light record = makeRecord(100, 0x00FFFFFF, 0);
            record.mData.mFlags |= ESM::Light::PulseSlow;

            const osg::ref_ptr<SceneUtil::LightSource> lamp = SceneUtil::createLightSource(
                SceneUtil::LightCommon(record), SceneUtil::Mask_Lighting, /*isExterior=*/false, osg::Vec4f(1, 1, 1, 1));

            // A pulse turns once in three seconds. Eight samples across it put one of them within an
            // eighth of a turn of the peak, so the deepest is at least `0.35 * cos(pi / 8)` from
            // rest — and every one of them carries the same ambient, which is the point.
            const osg::Vec3f white = lightColour(*makeGraphLight(osg::Vec4f(), osg::Vec4f(1, 1, 1, 1)), 0.0);

            float deepest = 0.0f;
            for (int i = 0; i < 8; ++i)
            {
                const osg::Vec3f lit = lightColour(*lamp, static_cast<double>(i) * 0.375);

                // The record's own white, decoded, plus the ambient that does not animate.
                const float diffuse = lit.x() - white.x();
                EXPECT_GT(diffuse, 0.0f) << "at sample " << i;

                deepest = std::max(deepest, std::abs(diffuse - 1.0f));
            }

            EXPECT_GT(deepest, 0.32f) << "the animation never ran";
        }

        /// The packing is `0xAABBGGRR`: red in the low byte.
        ///
        /// Reading it the other way round turns every candle in the game blue, which is the kind of
        /// wrong that looks deliberate.
        TEST(RtxLightBuilderTest, aColourIsRedFirstAndDecodedOutOfDisplaySpace)
        {
            EXPECT_EQ(decodeColour(0x00FFFFFF), osg::Vec3f(1.0f, 1.0f, 1.0f));
            EXPECT_EQ(decodeColour(0), osg::Vec3f(0.0f, 0.0f, 0.0f));

            const osg::Vec3f candle = decodeColour(0x000080FF);
            EXPECT_FLOAT_EQ(candle.x(), 1.0f) << "red is the low byte";
            EXPECT_EQ(candle.z(), 0.0f) << "and blue the third";

            // Mid grey is where the two spaces diverge most, so it is where skipping the decode is
            // most visible: 128 of 255 is 0.50196 encoded and
            // ((0.50196 + 0.055) / 1.055)^2.4 = 0.21586 linear.
            EXPECT_NEAR(candle.y(), 0.21586f, 1e-5f);
            EXPECT_NEAR(decodeColour(0x00808080).x(), 0.21586f, 1e-5f);
        }

        /// The colour the game hands over is the same colour, and takes the same decode.
        ///
        /// **OpenMW's own comment calls its pipeline linear and it is not the numbers it is talking
        /// about.** `SceneUtil::colourFromRGB` divides a record's bytes by 255 and stops, so what
        /// settles on a light, a fog or the sky is display-encoded exactly as the record was — which
        /// is why the game path decodes rather than passing it through, and why the two must land on
        /// the same value for the same record or a screenshot and the game are two different worlds.
        TEST(RtxLightBuilderTest, aColourTheGameHasAlreadyUnpackedDecodesToTheSameLight)
        {
            for (const std::uint32_t packed : { 0x00000000u, 0x00808080u, 0x000080FFu, 0x00FFFFFFu })
                EXPECT_EQ(decodeColour(packed), decodeColour(SceneUtil::colourFromRGB(packed))) << "packed " << packed;

            // The alpha is dropped rather than carried: nothing downstream of a light has a use for
            // one, and a fog colour arrives with its own.
            EXPECT_EQ(decodeColour(osg::Vec4f(1.0f, 1.0f, 1.0f, 0.25f)), osg::Vec3f(1.0f, 1.0f, 1.0f));

            // The same mid grey, reached the other way: 128 of 255 encoded is 0.21586 linear.
            EXPECT_NEAR(decodeColour(osg::Vec4f(128.0f / 255.0f, 0.0f, 0.0f, 1.0f)).x(), 0.21586f, 1e-5f);
        }

        /// What a light in the graph radiates is both of its terms, decoded — and it has to be the
        /// same answer the record gives, or a played frame and a screenshot are lit differently.
        ///
        /// **The ambient is not a second kind of light here.** A fixed-function pipeline had two
        /// terms because it had two things to do with them; a tracer has one. `setLightEffect` puts
        /// a glow light's whole colour in the ambient and leaves the diffuse at zero, so reading the
        /// diffuse alone is reading every Light spell in the game as unlit.
        TEST(RtxLightBuilderTest, aGraphLightRadiatesBothItsTermsAndTakesTheRecordsDecode)
        {
            // 128 of 255 is 0.50196 encoded, and ((0.50196 + 0.055) / 1.055)^2.4 = 0.21586 linear.
            const osg::Vec4f grey(128.0f / 255.0f, 0.0f, 0.0f, 1.0f);
            EXPECT_NEAR(lightColour(*makeGraphLight(grey, osg::Vec4f()), 0.0).x(), 0.21586f, 1e-5f);

            // What `Animation::setLightEffect` builds: nothing in the diffuse, 1.5 in the ambient.
            // ((1.5 + 0.055) / 1.055)^2.4 = 2.53716, and a walk reading the diffuse alone gets zero.
            const osg::Vec3f glow
                = lightColour(*makeGraphLight(osg::Vec4f(0, 0, 0, 0), osg::Vec4f(1.5f, 1.5f, 1.5f, 1)), 0.0);
            EXPECT_NEAR(glow.x(), 2.53716f, 1e-4f);
            EXPECT_NEAR(glow.z(), 2.53716f, 1e-4f);

            // Both at once add as light adds, after each is decoded and not before: 0.21586 of red
            // on top of 2.53716 of white.
            const osg::Vec3f both = lightColour(*makeGraphLight(grey, osg::Vec4f(1.5f, 1.5f, 1.5f, 1)), 0.0);
            EXPECT_NEAR(both.x(), 2.75302f, 1e-4f);
            EXPECT_NEAR(both.y(), 2.53716f, 1e-4f);

            // **The property the whole function exists for.** The harness reads a cell's `LIGH`
            // records and the game reads the `SceneUtil::LightSource` nodes its graph already holds;
            // for one record those two have to be one light, down to the last bit of the intensity.
            for (const std::uint32_t packed : { 0x00000000u, 0x00808080u, 0x000080FFu, 0x00FFFFFFu })
            {
                const ESM::Light record = makeRecord(100, packed, 0);
                const std::optional<Rtx::Light> fromRecord = makeLight(record, osg::Vec3f(1, 2, 3));

                const osg::ref_ptr<SceneUtil::LightSource> graph
                    = makeGraphLight(SceneUtil::colourFromRGB(packed), osg::Vec4f());
                const std::optional<Rtx::Light> fromGraph
                    = makeLight(lightColour(*graph, 0.0), 100.0f, osg::Vec3f(1, 2, 3));

                ASSERT_TRUE(fromRecord.has_value() && fromGraph.has_value()) << "packed " << packed;
                EXPECT_EQ(fromRecord->mIntensity, fromGraph->mIntensity) << "packed " << packed;
                EXPECT_EQ(fromRecord->mReach, fromGraph->mReach);
                EXPECT_EQ(fromRecord->mRadius, fromGraph->mRadius);
                EXPECT_EQ(fromRecord->mPosition, fromGraph->mPosition);
            }
        }

        /// Brightness, reach and the size of the flame all come off the one number the record
        /// carries, and part company.
        ///
        /// Intensity stays on the recorded radius, because that is what the lamp *is*. Only the
        /// falloff's run is stretched, because Morrowind's radii were tuned for a renderer where an
        /// ambient term lit the room and a lamp only had to light its own post. And the glowing part
        /// is a fraction of it, which is the same reading of the record as the intensity's: an
        /// emitter of fixed radiance is brighter by its area, so a lamp that is four times as bright
        /// is twice as wide and its shadows are twice as soft.
        TEST(RtxLightBuilderTest, intensityScalesWithTheRecordedRadiusAndReachIsStretchedPastIt)
        {
            const std::optional<Rtx::Light> light = makeLight(makeRecord(100, 0x00FFFFFF, 0), osg::Vec3f(1, 2, 3));

            ASSERT_TRUE(light.has_value());
            EXPECT_EQ(light->mPosition, osg::Vec3f(1, 2, 3));

            // 100 * 100 * 0.25 * pi = 7853.98, and white decodes to one.
            EXPECT_NEAR(light->mIntensity.x(), 7853.98f, 0.01f);
            EXPECT_NEAR(light->mIntensity.y(), 7853.98f, 0.01f);

            // 100 * 2 + 128.
            EXPECT_FLOAT_EQ(light->mReach, 328.0f);

            // A sixteenth of the record: 6.25 units, which is nine centimetres across at seventy
            // units to the metre — a flame, and not the metre and a half the reach describes.
            EXPECT_FLOAT_EQ(light->mRadius, 6.25f);

            // Doubling the radius quadruples the brightness, doubles the flame and rather less than
            // doubles the reach: 200 * 200 * 0.25 * pi = 31415.9, 200 / 16 = 12.5, and
            // 200 * 2 + 128 = 528.
            const std::optional<Rtx::Light> larger = makeLight(makeRecord(200, 0x00FFFFFF, 0), osg::Vec3f());
            ASSERT_TRUE(larger.has_value());
            EXPECT_NEAR(larger->mIntensity.x(), 31415.9f, 0.1f);
            EXPECT_FLOAT_EQ(larger->mReach, 528.0f);
            EXPECT_FLOAT_EQ(larger->mRadius, 12.5f);

            // The two readings of the record are one reading: an emitter of fixed radiance is
            // brighter by its area, so the brightness has to be the square of the size for a candle
            // and a brazier to be the same fire at two scales rather than two arbitrary lamps.
            EXPECT_NEAR(larger->mIntensity.x() / light->mIntensity.x(),
                (larger->mRadius / light->mRadius) * (larger->mRadius / light->mRadius), 1e-4f);
        }

        /// A surface that glows is given the lamp its glow comes to, and the lamp reaches as far as
        /// a recorded lamp of that brightness reaches.
        ///
        /// **A quarter of the area is Cauchy's theorem and not a guess**: a convex body's projected
        /// area averaged over every direction is a quarter of its surface, so radiance `L` over
        /// area `A` is `L * A / 4` per steradian on average. A quad of 400 square units at a
        /// radiance of (2, 1, 0) is then (200, 100, 0) — as bright as a white `LIGH` of radius
        /// `sqrt(200 / (0.25 pi))`, 15.958, which reaches `2 * 15.958 + 128 = 159.915`.
        TEST(RtxLightBuilderTest, aGlowingSurfaceIsGivenTheLampItsGlowComesTo)
        {
            const float equivalent = std::sqrt(200.0f / (0.25f * Shaders::PI));

            const std::optional<Light> lamp
                = emissiveLight(osg::Vec3f(2.0f, 1.0f, 0.0f), 400.0f, 7.5f, osg::Vec3f(1.0f, 2.0f, 3.0f));
            ASSERT_TRUE(lamp.has_value());
            EXPECT_EQ(lamp->mPosition, osg::Vec3f(1.0f, 2.0f, 3.0f));
            EXPECT_EQ(lamp->mIntensity, osg::Vec3f(200.0f, 100.0f, 0.0f));
            EXPECT_NEAR(lamp->mReach, 2.0f * equivalent + 128.0f, 1e-3f);
            EXPECT_NEAR(lamp->mReach, 159.915f, 1e-2f);
            EXPECT_EQ(lamp->mRadius, 7.5f) << "the glow's own extent, which its shadow rays stop short of";

            // The same brightness by a recorded lamp reaches the same distance, which is the rule.
            const std::optional<Light> recorded = makeLight(osg::Vec3f(1.0f, 0.5f, 0.0f), equivalent, osg::Vec3f());
            ASSERT_TRUE(recorded.has_value());
            EXPECT_NEAR(recorded->mIntensity.x(), 200.0f, 1e-3f);
            EXPECT_NEAR(recorded->mReach, lamp->mReach, 1e-3f);

            // Nothing glows, nothing is placed: a black surface, or one with no area.
            EXPECT_FALSE(emissiveLight(osg::Vec3f(), 400.0f, 7.5f, osg::Vec3f()).has_value());
            EXPECT_FALSE(emissiveLight(osg::Vec3f(2.0f, 1.0f, 0.0f), 0.0f, 7.5f, osg::Vec3f()).has_value());
        }

        /// The sun's arc, which is the engine's own and not an approximation of it.
        ///
        /// `(-400 * orbit, 75, -100)` with `orbit` running from one at sunrise to minus one at
        /// nightfall — so the vector is where the light *goes*, west at dawn and east at dusk, and
        /// Every quarter hour of the day, asked for.
        ///
        /// **A fallback key the game does not define throws rather than reading zero**, so this is a
        /// test that `makeDaylight` asks only for settings that exist. It did not: the land fog
        /// depth is recorded for day and night alone, and every hour inside sunrise or sunset asked
        /// for a third that was never written, which took the whole tool down.
        ///
        /// The times are seeded here because the phase boundaries come out of the same map, and an
        /// unseeded one puts sunrise and sunset on top of each other at midnight. `Fallback::Map`
        /// keeps the first value it is given for a key and a test elsewhere in this binary opens the
        /// real installation, so which values these reads got depends on the order the suite ran in
        /// — which is why what is pinned below is what is true of either.
        TEST(RtxLightBuilderTest, everyHourAsksOnlyForSettingsTheGameDefines)
        {
            Fallback::Map::init({
                { "Weather_Sunrise_Time", "6" },
                { "Weather_Sunset_Time", "18" },
                { "Weather_Sunset_Duration", "2" },
                { "Weather_Clear_Land_Fog_Day_Depth", "0.4" },
                { "Weather_Clear_Land_Fog_Night_Depth", "0.8" },
                { "Weather_Clear_Wind_Speed", "0.3" },
                { "Weather_Ashstorm_Wind_Speed", "0.8" },
                { "Weather_Clear_Sun_Disc_Sunset_Color", "255,189,157" },
                { "Weather_Clear_Glare_View", "1" },

                // The rest of what `requireWeather` asks a weather for, with the shipped numbers,
                // so a Clear the game defines is a Clear this test defines.
                { "Weather_Clear_Sky_Sunrise_Color", "117,141,164" },
                { "Weather_Clear_Sky_Day_Color", "095,135,203" },
                { "Weather_Clear_Sky_Sunset_Color", "056,089,129" },
                { "Weather_Clear_Sky_Night_Color", "009,010,011" },
                { "Weather_Clear_Fog_Sunrise_Color", "255,189,157" },
                { "Weather_Clear_Fog_Day_Color", "206,227,255" },
                { "Weather_Clear_Fog_Sunset_Color", "255,189,157" },
                { "Weather_Clear_Fog_Night_Color", "009,010,011" },
                { "Weather_Clear_Ambient_Sunrise_Color", "047,066,096" },
                { "Weather_Clear_Ambient_Day_Color", "137,140,160" },
                { "Weather_Clear_Ambient_Sunset_Color", "068,075,096" },
                { "Weather_Clear_Ambient_Night_Color", "032,035,042" },
                { "Weather_Clear_Cloud_Texture", "Tx_Sky_Clear.dds" },
                { "Weather_Clear_Cloud_Speed", "1.25" },
                { "Weather_Clear_Clouds_Maximum_Percent", "1.0" },

                // The sun's own ramp, seeded with the shipped numbers so that the two ways this
                // test can be run — against these or against a real installation — agree. The night
                // value being the blue one is what the disc is here to not be painted with.
                { "Weather_Clear_Sun_Sunrise_Color", "242,159,119" },
                { "Weather_Clear_Sun_Day_Color", "255,252,238" },
                { "Weather_Clear_Sun_Sunset_Color", "255,114,079" },
                { "Weather_Clear_Sun_Night_Color", "059,097,176" },
            });

            for (float hour = 0.0f; hour < 24.0f; hour += 0.25f)
                EXPECT_NO_THROW(makeDaylight("Clear", hour)) << "at hour " << hour;

            // A file records one depth for daylight and one for night, and the ramp hands the day
            // value to three of its four points. Deeper fog is thicker air, so the night value being
            // the larger of the two is what makes these comparisons say different things.
            const float day = makeDaylight("Clear", 12.0f).mFog.mExtinction;
            const float night = makeDaylight("Clear", 0.0f).mFog.mExtinction;
            EXPECT_GT(night, day);
            EXPECT_EQ(makeDaylight("Clear", 6.0f).mFog.mExtinction, day) << "sunrise reads the day depth";
            EXPECT_EQ(makeDaylight("Clear", 20.0f).mFog.mExtinction, night) << "and night begins at twenty";

            // And the weather reaches its air through `exteriorFog` rather than assembling one,
            // which is what keeps the extinction and the edge measured over one reach.
            EXPECT_EQ(makeDaylight("Clear", 12.0f).mFog.mEdge, distantLandReach());

            // **Dusk is between the two rather than one of them**, which is the whole of what the
            // engine's own ramp buys over reading whichever phase an hour falls in: the seeded
            // sunset runs from eighteen to twenty, so half past seven is halfway across it.
            const float dusk = makeDaylight("Clear", 19.5f).mFog.mExtinction;
            EXPECT_GT(dusk, day);
            EXPECT_LT(dusk, night);

            // **A night has no sun in it at all**, which is one fact rather than the engine's two.
            // Morrowind never switches its sunlight off — `WeatherManager` reads a colour off the
            // same ramp all night and turns off only the sprite — and a tracer that kept that light
            // cast hard shadows swinging back across the ground until dawn, from a disc nothing was
            // drawing. There is no second field left to say otherwise.
            EXPECT_NE(makeDaylight("Clear", 12.0f).mSun.mIrradiance, osg::Vec3f()) << "noon";
            EXPECT_EQ(makeDaylight("Clear", 0.0f).mSun.mIrradiance, osg::Vec3f()) << "midnight";
            EXPECT_EQ(makeDaylight("Clear", 22.0f).mSun.mIrradiance, osg::Vec3f()) << "night begins at twenty";
            EXPECT_NE(makeDaylight("Clear", 7.0f).mSun.mIrradiance, osg::Vec3f()) << "and it is back after six";

            // The disc is white for every hour the sun is up and only warms on the way down, which
            // is the one thing the light never does.
            for (const float hour : { 6.5f, 9.0f, 12.0f, 15.0f })
                EXPECT_EQ(makeDaylight("Clear", hour).mSun.mDiscColour, osg::Vec3f(1.0f, 1.0f, 1.0f))
                    << "at hour " << hour;

            // Half past seven and not eighteen: the disc's colour is summed with the ambient and
            // clipped, the way the original did it, and at the start of sunset the shipped ambient
            // is still bright enough to clip all three channels to white. It warms once the
            // ambient has gone down with it.
            const Daylight down = makeDaylight("Clear", 19.5f);
            EXPECT_FLOAT_EQ(down.mSun.mDiscColour.x(), 1.0f);
            EXPECT_LT(down.mSun.mDiscColour.z(), down.mSun.mDiscColour.x()) << "warm on the way down, never blue";

            // The wind comes off the same file and a key per weather, so a storm reading harder
            // than fair weather is what says the name reached the lookup rather than a constant
            // being handed back.
            //
            // **Compared rather than pinned.** The seeds above are 0.3 and 0.8, but a test elsewhere
            // in this binary opens the real installation and `Fallback::Map::init` keeps whichever
            // value landed first — so which pair this reads depends on the order the suite ran in,
            // and only the inequality is true of both.
            EXPECT_GT(Weather::windSpeed("Ashstorm"), Weather::windSpeed("Clear"));
            EXPECT_GT(Weather::windSpeed("Clear"), 0.0f);

            // A name that is none of the ten is not a key the map will even consider, which is why
            // `weatherIndex` is the thing to ask first.
            EXPECT_THROW(makeDaylight("Drizzle", 12.0f), std::logic_error);
        }

        /// A weather the configuration left out is refused by name, before it can read as nought.
        ///
        /// **Every key the picture reads of a weather, removed one at a time**: each refusal names
        /// the weather and the key, and with all of them present nothing is refused. Over tables of
        /// the test's own rather than `Fallback::Map`'s, because that map keeps the first value it
        /// is given and another test in this binary may already have given it the real ones — which
        /// after the harness's configuration gained its two missing weathers is every one of the ten.
        TEST(RtxLightBuilderTest, aWeatherTheConfigurationLeftOutIsRefusedByName)
        {
            constexpr std::array<std::string_view, 18> colours = { "Sky_Sunrise_Color", "Sky_Day_Color",
                "Sky_Sunset_Color", "Sky_Night_Color", "Fog_Sunrise_Color", "Fog_Day_Color", "Fog_Sunset_Color",
                "Fog_Night_Color", "Ambient_Sunrise_Color", "Ambient_Day_Color", "Ambient_Sunset_Color",
                "Ambient_Night_Color", "Sun_Sunrise_Color", "Sun_Day_Color", "Sun_Sunset_Color", "Sun_Night_Color",
                "Sun_Disc_Sunset_Color", "Cloud_Texture" };
            constexpr std::array<std::string_view, 6> numbers = { "Land_Fog_Day_Depth", "Land_Fog_Night_Depth",
                "Glare_View", "Wind_Speed", "Cloud_Speed", "Clouds_Maximum_Percent" };

            std::map<std::string, std::string, std::less<>> strings;
            for (const std::string_view colour : colours)
                strings["Weather_Blight_" + std::string(colour)] = "128,019,019";

            std::map<std::string, float, std::less<>> floats;
            for (const std::string_view number : numbers)
                floats["Weather_Blight_" + std::string(number)] = 1.0f;

            EXPECT_NO_THROW(requireWeather("Blight", floats, strings));

            const auto refusedFor = [](std::string_view key, const auto& check) {
                try
                {
                    check();
                    ADD_FAILURE() << "nothing refused " << key;
                }
                catch (const Error& error)
                {
                    const std::string what = error.what();
                    EXPECT_NE(what.find("Blight"), std::string::npos) << what;
                    EXPECT_NE(what.find(key), std::string::npos) << what;
                }
            };

            for (const std::string_view colour : colours)
            {
                auto without = strings;
                const std::string key = "Weather_Blight_" + std::string(colour);
                without.erase(key);
                refusedFor(key, [&] { requireWeather("Blight", floats, without); });
            }

            for (const std::string_view number : numbers)
            {
                auto without = floats;
                const std::string key = "Weather_Blight_" + std::string(number);
                without.erase(key);
                refusedFor(key, [&] { requireWeather("Blight", without, strings); });
            }
        }

        /// A sun below the horizon is not a sun, in either term.
        ///
        /// **This is the one rule, and it is here so that a renderer cannot be written without it.**
        /// Every sun bug this file has seen was the same shape — the engine keeps five independent
        /// dials for one sun and its rasterizer never had to make two of them agree, so a tracer
        /// that carried them across got a light coming from one place, a disc drawn in another, and
        /// a shadow cast at an hour when nothing was drawn at all. `makeSkylight` is the only way to
        /// build one, and there is nothing it can be handed that says the incoherent thing.
        TEST(RtxSkylightTest, aSunBelowTheHorizonLightsNothingInEitherTerm)
        {
            const osg::Vec3f up(0.0f, 0.0f, 1.0f);
            const osg::Vec3f blue(0.05f, 0.12f, 0.44f); ///< what `Sun_Night_Color` decodes to
            const osg::Vec3f room(0.01f, 0.011f, 0.013f);

            const auto at = [&](float share) {
                return makeSkylight(
                    SkyReading{ .mSunPosition = up, .mSunShare = share, .mSunColour = blue, .mAmbient = room });
            };

            // **Nothing at all when there is no sun**, and it is the irradiance that says so, since
            // that is the one thing every use of the sun downstream is gated on.
            EXPECT_EQ(at(0.0f).mSun.mIrradiance, osg::Vec3f());

            // The whole of it when there is, on the shared sun-to-sky scale.
            EXPECT_EQ(at(1.0f).mSun.mIrradiance, blue * Rtx::Shaders::DAYLIGHT);

            // **And the fill is a dusk's, not a night's.** The sun's light with its direction taken
            // away is a thing only an hour with a sun in it has — so this is nothing at either end
            // and largest where the disc straddles the horizon.
            //
            // **The shape that suggests itself is `1 - share`, and it is largest where there is no sun.**
            // Morrowind leaves a blue in the sun's slot all night, and it is the original engine's
            // stand-in for moonlight; spread as an ambient it came to six times the night ambient the
            // weather itself records, flat and shadowless, on every surface. This renderer traces the
            // moons, so keeping it was the moon counted twice and a night that did not read as one.
            EXPECT_EQ(at(0.0f).mAmbient, room) << "a night's ambient is the weather's own";
            EXPECT_EQ(at(1.0f).mAmbient, room) << "and a day's is untouched";

            // Half set: `2 * 0.5 * 0.5` is a half of a quarter of the irradiance over pi.
            const osg::Vec3f half = blue * Rtx::Shaders::DAYLIGHT * (0.5f * 0.25f / Rtx::Shaders::PI);
            for (int channel = 0; channel < 3; ++channel)
                EXPECT_NEAR(at(0.5f).mAmbient[channel], (room + half)[channel], 1e-6f);

            // And it only ever adds, at every share between the two ends.
            for (const float share : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
                for (int channel = 0; channel < 3; ++channel)
                    EXPECT_GE(at(share).mAmbient[channel], room[channel]) << "at share " << share;

            // The position stays whatever it was, because a moon's crescent points at where the sun
            // would be and that is a different question from whether it is there.
            EXPECT_EQ(at(0.0f).mSun.mPosition, up);

            // A weather that hides the sun paints a paler disc, and says nothing about whether there
            // is one: the glare reaches only the colour.
            const Skylight overcast = makeSkylight(SkyReading{ .mSunPosition = up,
                .mSunShare = 1.0f,
                .mSunColour = blue,
                .mAmbient = room,
                .mDiscColour = osg::Vec3f(1.0f, 1.0f, 1.0f),
                .mGlare = 0.25f });
            EXPECT_EQ(overcast.mSun.mDiscColour, osg::Vec3f(0.25f, 0.25f, 0.25f));
            EXPECT_EQ(overcast.mSun.mIrradiance, blue * Rtx::Shaders::DAYLIGHT) << "and lights the same";

            // **The same weather read twice, at two shares.** A cloud deck stands above the ground's
            // horizon and keeps the sun after it has set down here, so it takes the same sun at its
            // own share — the same place, the same colour, and only the quantity apart.
            const Skylight dusk = makeSkylight(SkyReading{
                .mSunPosition = up, .mSunShare = 0.0f, .mSunShareAloft = 0.25f, .mSunColour = blue, .mAmbient = room });

            EXPECT_EQ(dusk.mSun.mIrradiance, osg::Vec3f()) << "the ground has lost it";
            EXPECT_EQ(dusk.mSunAloft.mIrradiance, blue * (0.25f * Rtx::Shaders::DAYLIGHT));
            EXPECT_EQ(dusk.mSunAloft.mPosition, dusk.mSun.mPosition);
            EXPECT_EQ(dusk.mSunAloft.mDiscColour, dusk.mSun.mDiscColour);

            // **And the ambient is the ground's alone.** What a layer above it keeps is that layer's
            // to spend; spreading it over the world as well would be the same light twice.
            EXPECT_EQ(dusk.mAmbient, room);
        }

        /// A layer over the ground keeps the sun, and what it keeps is an hour and not an angle.
        ///
        /// **Morrowind's sunset is a clock.** `Sky::sunShareAt` ramps between `mDayEnd` and
        /// `mNightStart` and nothing anywhere takes an elevation, so a layer that sees the sun 0.718
        /// degrees longer is handed the ramp read 5.35 minutes earlier — the time the disc takes to
        /// fall that far at 8.04 degrees an hour.
        ///
        /// **And the day is widened at both ends rather than moved**: a layer that sees the sun lower
        /// catches the sunrise early too, so the same offset runs the other way before noon.
        ///
        /// With Morrowind's own hours: at 19:00 the ground has 0.750 of the sun and the layer 0.793;
        /// at 20:00 the ground has none and the layer still holds 0.0872; and five minutes later the
        /// layer has none either.
        TEST(RtxSunAloftTest, aLayerOverTheGroundKeepsTheSunAfterItHasLostIt)
        {
            Sky::TimeOfDaySettings times{};
            times.mNightEnd = 6.0f;
            times.mDayStart = 8.0f;
            times.mDayEnd = 18.0f;
            times.mNightStart = 20.0f;

            EXPECT_NEAR(sunShareAloft(19.0f, times), 0.792623f, 1.0e-5f);
            EXPECT_NEAR(sunShareAloft(20.0f, times), 0.087237f, 1.0e-5f);
            EXPECT_EQ(sunShareAloft(20.1f, times), 0.0f) << "and it goes out too, five minutes later";

            // And the same offset the other way at dawn: at 06:05 the ground has a twentieth of the
            // sun and the layer has 0.139, because it caught the sunrise five minutes ago.
            EXPECT_NEAR(sunShareAloft(6.05f, times), 0.139227f, 1.0e-5f);

            // Nothing at all is different while the whole disc is up, which is every hour of the day
            // between the two ramps.
            for (const float hour : { 9.0f, 12.0f, 17.9f })
                EXPECT_EQ(sunShareAloft(hour, times), Sky::sunShareAt(hour, times)) << "at hour " << hour;

            // **And never less than the ground's**, at any hour of the clock — which is the whole
            // claim, and the one a sign error in the offset would break.
            for (float hour = 0.0f; hour < 24.0f; hour += 0.05f)
                EXPECT_GE(sunShareAloft(hour, times), Sky::sunShareAt(hour, times)) << "at hour " << hour;
        }

        /// The ten names, in the order a script id counts along.
        ///
        /// **This order is the engine's and not ours.** `MWWorld::WeatherManager::addWeather` is
        /// called ten times in `apps/openmw/mwworld/weather.cpp:672` and each call's position is the
        /// `mScriptId` the game later hands the renderer; the shader's `WEATHER_*` name the same
        /// positions. A table that drifted from either would put an ashstorm's sky over a rainstorm
        /// without anything failing to compile.
        TEST(RtxLightBuilderTest, aWeatherNameIndexesTheOrderTheEngineRegistersThemIn)
        {
            EXPECT_EQ(weatherIndex("Clear"), Rtx::Shaders::WEATHER_CLEAR);
            EXPECT_EQ(weatherIndex("Cloudy"), Rtx::Shaders::WEATHER_CLOUDY);
            EXPECT_EQ(weatherIndex("Foggy"), Rtx::Shaders::WEATHER_FOGGY);
            EXPECT_EQ(weatherIndex("Overcast"), Rtx::Shaders::WEATHER_OVERCAST);
            EXPECT_EQ(weatherIndex("Rain"), Rtx::Shaders::WEATHER_RAIN);
            EXPECT_EQ(weatherIndex("Thunderstorm"), Rtx::Shaders::WEATHER_THUNDERSTORM);
            EXPECT_EQ(weatherIndex("Ashstorm"), Rtx::Shaders::WEATHER_ASHSTORM);
            EXPECT_EQ(weatherIndex("Blight"), Rtx::Shaders::WEATHER_BLIGHT);
            EXPECT_EQ(weatherIndex("Snow"), Rtx::Shaders::WEATHER_SNOW);
            EXPECT_EQ(weatherIndex("Blizzard"), Rtx::Shaders::WEATHER_BLIZZARD);

            EXPECT_FALSE(weatherIndex("Drizzle").has_value());

            // **Case is not folded**, because the name goes on to spell a `Weather_<name>_*` key
            // and the fallback map's whitelist holds exactly one spelling of each. Accepting a
            // second here would hand `makeDaylight` a name that throws.
            EXPECT_FALSE(weatherIndex("clear").has_value());
            EXPECT_FALSE(weatherIndex("").has_value());
        }

        /// A region is offered only the weathers it ever gets.
        ///
        /// **A `REGN` record's ten chances add to a hundred and a zero means never**, in the order
        /// `WEATHER_*` names them. The Bitter Coast has no ashstorms and the Ashlands no snow, so a
        /// window that walked all ten would offer skies the game could not produce there.
        TEST(RtxLightBuilderTest, steppingTheWeatherSkipsTheOnesTheRegionNeverGets)
        {
            // Clear, Cloudy and Rain only — the shape of a coastal region, with everything from
            // Thunderstorm on left at nothing.
            ESM::Region coast;
            coast.mData.mProbabilities = { 50, 30, 0, 0, 20, 0, 0, 0, 0, 0 };

            EXPECT_EQ(nextRegionWeather(&coast, Rtx::Shaders::WEATHER_CLEAR, true), Rtx::Shaders::WEATHER_CLOUDY);
            EXPECT_EQ(nextRegionWeather(&coast, Rtx::Shaders::WEATHER_CLOUDY, true), Rtx::Shaders::WEATHER_RAIN)
                << "Foggy and Overcast are skipped";
            EXPECT_EQ(nextRegionWeather(&coast, Rtx::Shaders::WEATHER_RAIN, true), Rtx::Shaders::WEATHER_CLEAR)
                << "and it wraps past the six it never gets";

            // Backwards over the same three.
            EXPECT_EQ(nextRegionWeather(&coast, Rtx::Shaders::WEATHER_CLEAR, false), Rtx::Shaders::WEATHER_RAIN);
            EXPECT_EQ(nextRegionWeather(&coast, Rtx::Shaders::WEATHER_RAIN, false), Rtx::Shaders::WEATHER_CLOUDY);
            EXPECT_EQ(nextRegionWeather(&coast, Rtx::Shaders::WEATHER_CLOUDY, false), Rtx::Shaders::WEATHER_CLEAR);

            // **A step from a weather the region does not get still lands on one it does**, which is
            // what a camera crossing out of one region into another leaves behind.
            EXPECT_EQ(nextRegionWeather(&coast, Rtx::Shaders::WEATHER_BLIZZARD, true), Rtx::Shaders::WEATHER_CLEAR);

            // No region — an interior, or a cell naming one nothing defines — offers all ten.
            EXPECT_EQ(nextRegionWeather(nullptr, Rtx::Shaders::WEATHER_CLEAR, true), Rtx::Shaders::WEATHER_CLOUDY);
            EXPECT_EQ(nextRegionWeather(nullptr, Rtx::Shaders::WEATHER_CLEAR, false), Rtx::Shaders::WEATHER_BLIZZARD);

            // And a record that allows nothing at all steps once rather than spinning for ever.
            ESM::Region nowhere;
            nowhere.mData.mProbabilities = {};
            EXPECT_EQ(nextRegionWeather(&nowhere, Rtx::Shaders::WEATHER_CLEAR, true), Rtx::Shaders::WEATHER_CLOUDY);
        }

        /// Ash and blight blow off Red Mountain at whoever is standing in them.
        ///
        /// `apps/openmw/mwworld/weather.cpp:47` takes the direction from the volcano at (25000,
        /// 70000) to the player, flattened to the ground. Every other weather leaves it due north,
        /// which is `Weather::defaultStormDirection`.
        /// The hour holds an exposure back, and a noon does not.
        ///
        /// **The histogram cannot tell a midnight from a noon**, because it normalises whatever it
        /// is shown toward the key — so a renderer left to measure its own exposure has no night in
        /// it at any hour. The weather knows the hour absolutely, and this is what it says about it.
        TEST(RtxLightBuilderTest, theHourHoldsAnExposureBackAndANoonDoesNot)
        {
            // A full sun's worth of light is the hour the bias leaves alone, and anything past it is
            // held at one: an hour is allowed to darken a frame and never to open it.
            EXPECT_FLOAT_EQ(exposureBias(osg::Vec3f(8.0f, 8.0f, 8.0f), osg::Vec3f()), 1.0f);
            EXPECT_FLOAT_EQ(exposureBias(osg::Vec3f(80.0f, 80.0f, 80.0f), osg::Vec3f()), 1.0f);

            // A clear midnight: no sun at all, and the weather's own night ambient, which comes to
            // 0.0168 by luminance. `(0.0168 / 8)^0.314 = 0.14428`, which is the two stops and four
            // fifths the exponent comes to.
            EXPECT_NEAR(exposureBias(osg::Vec3f(), osg::Vec3f(0.0168f, 0.0168f, 0.0168f)), 0.14428f, 1e-4f);

            // And it only ever moves one way, so no hour is darker in the picture than a darker one.
            float darker = 0.0f;
            for (const float level : { 0.01f, 0.1f, 1.0f, 4.0f, 8.0f })
            {
                const float bias = exposureBias(osg::Vec3f(level, level, level), osg::Vec3f());
                EXPECT_GT(bias, darker) << "at a level of " << level;
                darker = bias;
            }
        }

        /// The air takes a body in the sky out as it goes down, and takes the blue out first.
        ///
        /// **Two published figures and nothing else.** Rayleigh optical depth at the three sRGB
        /// primaries is 0.0683, 0.0973 and 0.2213 — `0.008569 λ^-4` with its usual correction, at
        /// 600, 550 and 450 nanometres — and Kasten and Young's air mass runs from 0.9997 overhead
        /// to 37.92 at the horizon. The transmittance is `exp(-depth * mass)`, worked out below by
        /// hand.
        TEST(RtxLightBuilderTest, theAirTakesABodyOutAsItGoesDown)
        {
            // Overhead: one air mass, so the depths come through as they are. A ninth of the green
            // goes even there, which is the price a full moon at the zenith pays.
            const osg::Vec3f zenith = airTransmittance(1.0f);
            EXPECT_NEAR(zenith.x(), 0.9340f, 1e-4f);
            EXPECT_NEAR(zenith.y(), 0.9073f, 1e-4f);
            EXPECT_NEAR(zenith.z(), 0.8015f, 1e-4f);

            // Thirty degrees is 1.9943 air masses, which is where a moon is most of the way to
            // itself.
            const osg::Vec3f third = airTransmittance(0.5f);
            EXPECT_NEAR(third.x(), 0.8727f, 1e-4f);
            EXPECT_NEAR(third.y(), 0.8236f, 1e-4f);
            EXPECT_NEAR(third.z(), 0.6432f, 1e-4f);

            // The horizon is 37.92 of them, and blue does not survive it: three parts in ten
            // thousand against a thirteenth of the red.
            const osg::Vec3f edge = airTransmittance(0.0f);
            EXPECT_NEAR(edge.x(), 0.0750f, 1e-4f);
            EXPECT_NEAR(edge.y(), 0.0250f, 1e-4f);
            EXPECT_NEAR(edge.z(), 0.000226f, 1e-6f);

            // Below the horizon there is no slant path to measure, so it holds at the horizon's own
            // rather than taking a root of a negative angle.
            EXPECT_EQ(airTransmittance(-0.5f), edge);

            // And it only ever rises, which is what keeps a body from brightening as it sets.
            float below = 0.0f;
            for (int step = 0; step <= 64; ++step)
            {
                const float carried = airTransmittance(float(step) / 64.0f).y();
                EXPECT_GT(carried, below) << "at a height of " << float(step) / 64.0f;
                below = carried;
            }
        }

        /// A night's sky lights with more than it is drawn with, and a day's does not.
        ///
        /// **Because Morrowind states the two in different places.** It lights a night by putting
        /// `Ambient_<weather>_Night_Color` on every surface and it draws that night by
        /// `Sky_<weather>_Night_Color`, and for a clear night the first is 0.0168 by luminance
        /// against the second's 0.0030. A renderer that lights the ground by tracing the sky is short
        /// by the difference, which is the whole of what this makes up.
        TEST(RtxLightBuilderTest, aNightsSkyLightsWithMoreThanItIsDrawnWith)
        {
            const auto grey = [](float value) { return osg::Vec3f(value, value, value); };
            const auto filled = [&](float horizon, float zenith, float sheets, float ambient) {
                return skyBudget(grey(horizon), grey(zenith), grey(sheets), grey(ambient)).mFill.x();
            };

            // A gradient linear in `sin(elevation)` delivers what a uniform sky of `h / 3 + 2z / 3`
            // would, so a horizon of 0.3 under a zenith of 0.6 is worth 0.5 — and an ambient of 0.8
            // asks for the 0.3 that is left.
            EXPECT_NEAR(filled(0.3f, 0.6f, 0.0f, 0.8f), 0.3f, 1e-6f);

            // The zenith is worth twice the horizon, which is what makes those two different skies:
            // the same pair the other way up is worth 0.4 and leaves 0.4 to ask for.
            EXPECT_NEAR(filled(0.6f, 0.3f, 0.0f, 0.8f), 0.4f, 1e-6f);

            // A sky that already carries the ambient asks for nothing, and one that outruns it does
            // not ask for less than nothing.
            EXPECT_EQ(filled(0.5f, 0.5f, 0.0f, 0.5f), 0.0f);
            EXPECT_EQ(filled(0.9f, 0.9f, 0.0f, 0.2f), 0.0f);

            // **The night's own sheets come out of the same figure**, which is what keeps a night
            // where it was as one more layer starts lighting: the stars used to light nothing, and a
            // tenth of the 0.3 above now comes from them instead of from the fill.
            EXPECT_NEAR(filled(0.3f, 0.6f, 0.03f, 0.8f), 0.27f, 1e-6f);
            EXPECT_EQ(filled(0.3f, 0.6f, 0.5f, 0.8f), 0.0f) << "and sheets that outrun it ask for nothing";

            // And it asks per channel: a red ambient over a grey sky fills the red alone rather than
            // lifting the whole of it.
            const SkyBudget tinted = skyBudget(grey(0.5f), grey(0.5f), osg::Vec3f(), osg::Vec3f(0.9f, 0.5f, 0.1f));
            EXPECT_NEAR(tinted.mFill.x(), 0.4f, 1e-6f);
            EXPECT_EQ(tinted.mFill.y(), 0.0f);
            EXPECT_EQ(tinted.mFill.z(), 0.0f);

            // **And the mean beside it is the whole of what the sky delivers**, fill and all — which
            // is what a cloud deck hangs under. The sky carries 0.5 by itself, so the red channel
            // reaches the 0.9 its ambient asks for and the other two stay at what the sky is.
            EXPECT_NEAR(tinted.mMean.x(), 0.9f, 1e-6f);
            EXPECT_NEAR(tinted.mMean.y(), 0.5f, 1e-6f);
            EXPECT_NEAR(tinted.mMean.z(), 0.5f, 1e-6f);
        }

        /// And the weather fills it, so a frame gets the hour it is at rather than a default.
        ///
        /// **The relation and not a number.** Which weather values this binary sees depends on
        /// whether a test before it opened the real installation, which
        /// `everyHourAsksOnlyForSettingsTheGameDefines` says more about. What holds either way is
        /// that the field is this hour's own light put through the curve rather than a default left
        /// standing.
        TEST(RtxLightBuilderTest, aDaylightCarriesTheHoursOwnBias)
        {
            for (const float hour : { 0.0f, 6.0f, 12.0f, 18.0f })
            {
                const Daylight day = makeDaylight("Clear", hour);
                EXPECT_FLOAT_EQ(day.mExposureBias, exposureBias(day.mSun.mIrradiance, day.mAmbient))
                    << "at hour " << hour;
            }
        }

        /// A room's `AMBI` record, as Berandas, Propylon Chamber writes it: ambient `15, 15, 15`,
        /// sunlight `10, 16, 16`, fog `15, 21, 21` at a depth of one. Red is the low byte.
        ESM::Cell::AMBIstruct makeRoom(std::uint32_t ambient, std::uint32_t sunlight, std::uint32_t fog)
        {
            return ESM::Cell::AMBIstruct{
                .mAmbient = ambient, .mSunlight = sunlight, .mFog = fog, .mFogDensity = 1.0f
            };
        }

        /// A room is lit by its own record under the engine's sun, and nothing of the hour reaches it.
        ///
        /// **The sunlight is a sun.** At full share the direct term is the whole of `DAYLIGHT` times
        /// the decoded colour and the dusk term is nought, so the ambient is the record's exactly;
        /// the sky is the fog at both ends, because a room has no dome. A record with no sunlight
        /// gives a room with no sun, which is what makes the first row the record's doing.
        TEST(RtxRoomLightTest, aRoomIsLitByItsRecordUnderTheEnginesSun)
        {
            const ESM::Cell::AMBIstruct chamber = makeRoom(0x000F0F0F, 0x0010100A, 0x0015150F);
            const Daylight room = makeRoomLight(chamber);

            const osg::Vec3f sunlight = decodeColour(0x0010100Au);
            EXPECT_EQ(room.mSun.mPosition, Sky::roomSun().mPosition);
            EXPECT_FLOAT_EQ(room.mSun.mIrradiance.x(), sunlight.x() * Shaders::DAYLIGHT);
            EXPECT_FLOAT_EQ(room.mSun.mIrradiance.y(), sunlight.y() * Shaders::DAYLIGHT);
            EXPECT_FLOAT_EQ(room.mSun.mIrradiance.z(), sunlight.z() * Shaders::DAYLIGHT);
            EXPECT_GT(room.mSun.mIrradiance.length2(), 0.0f) << "the chamber's sunlight is dim, not absent";

            EXPECT_EQ(room.mAmbient, decodeColour(0x000F0F0Fu));
            EXPECT_EQ(room.mSkyHorizon, decodeColour(0x0015150Fu));
            EXPECT_EQ(room.mSkyZenith, room.mSkyHorizon);

            const Fog air = roomFog(decodeColour(0x0015150Fu), 1.0f);
            EXPECT_EQ(room.mFog.mColour, air.mColour);
            EXPECT_FLOAT_EQ(room.mFog.mExtinction, air.mExtinction);

            EXPECT_FLOAT_EQ(room.mStarFade, 0.0f);
            EXPECT_FLOAT_EQ(room.mExposureBias, 1.0f) << "the game holds a room at one";

            // The record decides: the same room with its sunlight written black has no sun.
            const Daylight unlit = makeRoomLight(makeRoom(0x000F0F0F, 0x00000000, 0x0015150F));
            EXPECT_EQ(unlit.mSun.mIrradiance, osg::Vec3f(0.0f, 0.0f, 0.0f));
            EXPECT_EQ(unlit.mAmbient, room.mAmbient);

            // **Night-Eye is added where the game adds it**: to the file's own numbers, before the
            // decode. `15 / 255 + 0.35 = 0.40882`, and `((0.40882 + 0.055) / 1.055)^2.4 = 0.13914`.
            const Daylight seen = makeRoomLight(chamber, osg::Vec3f(0.35f, 0.35f, 0.35f));
            EXPECT_NEAR(seen.mAmbient.x(), 0.13914f, 2e-4f);
            EXPECT_EQ(seen.mSun.mIrradiance, room.mSun.mIrradiance) << "the effect is an ambient and not a sun";

            // **A cell that wrote no record is a black room**, in the game and here: its `mAmbi` is
            // the zeros the loader left, and both hosts hand those over rather than checking
            // `mHasAmbi` first.
            ESM::Cell unwritten;
            unwritten.mHasAmbi = false;
            const Daylight bare = makeRoomLight(unwritten.mAmbi);
            EXPECT_EQ(bare.mSun.mIrradiance, osg::Vec3f(0.0f, 0.0f, 0.0f));
            EXPECT_EQ(bare.mAmbient, osg::Vec3f(0.0f, 0.0f, 0.0f));
            EXPECT_FLOAT_EQ(bare.mFog.mExtinction, 0.0f);
        }

        TEST(RtxLightBuilderTest, anAshStormBlowsAwayFromRedMountainAndNothingElseTurnsAtAll)
        {
            const osg::Vec3f north(0.0f, 1.0f, 0.0f);

            // A three-four-five triangle off the summit, so the unit vector is exact: (3, 4) over a
            // length of 5 is (0.6, 0.8). The height is thrown away rather than normalised with the
            // rest, which is what keeps the wind on the ground.
            const osg::Vec3f standing(25003.0f, 70004.0f, 999.0f);
            for (const std::uint32_t weather : { Rtx::Shaders::WEATHER_ASHSTORM, Rtx::Shaders::WEATHER_BLIGHT })
            {
                const osg::Vec3f blowing = stormDirection(weather, standing);
                EXPECT_FLOAT_EQ(blowing.x(), 0.6f) << "weather " << weather;
                EXPECT_FLOAT_EQ(blowing.y(), 0.8f) << "weather " << weather;
                EXPECT_FLOAT_EQ(blowing.z(), 0.0f) << "weather " << weather;
            }

            // Due south of the mountain it points south, which is the half of "away from" that a
            // fixed bearing would get wrong.
            EXPECT_EQ(stormDirection(Rtx::Shaders::WEATHER_ASHSTORM, osg::Vec3f(25000.0f, 60000.0f, 0.0f)),
                osg::Vec3f(0.0f, -1.0f, 0.0f));

            // Standing on the summit there is no away, and a normalised zero is a frame of NaN.
            EXPECT_EQ(stormDirection(Rtx::Shaders::WEATHER_ASHSTORM, osg::Vec3f(25000.0f, 70000.0f, 4000.0f)), north);

            // Everything the mountain does not send reads the wind's own bearing wherever it stands.
            for (const std::uint32_t weather : { Rtx::Shaders::WEATHER_CLEAR, Rtx::Shaders::WEATHER_RAIN,
                     Rtx::Shaders::WEATHER_BLIZZARD, Rtx::Shaders::WEATHER_SNOW })
                EXPECT_EQ(stormDirection(weather, standing), north) << "weather " << weather;
        }

        /// An unlit record places a mesh and no light, a negative one is nonsense, and a carryable
        /// one burns where it lies.
        ///
        /// **Carryable is not carried.** A hundred and fifty-one of `Morrowind.esm`'s light records
        /// can be picked up — every candle and torch among them — and the game lights a cell with
        /// the ones lying in it: `MWClass::Light::insertObjectRendering` withholds a light source
        /// for `OffDefault` and for nothing else. Refusing `Carry` here was every candle on every
        /// table gone dark by the record route, while the graph route lit them.
        TEST(RtxLightBuilderTest, anUnlitRecordCastsNothingAndACarryableOneBurnsWhereItLies)
        {
            EXPECT_FALSE(castsWherePlaced(makeRecord(100, 0x00FFFFFF, ESM::Light::OffDefault)));
            EXPECT_FALSE(makeLight(makeRecord(100, 0x00FFFFFF, ESM::Light::OffDefault), osg::Vec3f()).has_value());

            EXPECT_FALSE(makeLight(makeRecord(100, 0x00FFFFFF, ESM::Light::Negative), osg::Vec3f()).has_value());

            // The flags that say what a light is or how it animates leave it burning.
            for (const std::int32_t flag :
                { ESM::Light::Carry, ESM::Light::Dynamic, ESM::Light::Flicker, ESM::Light::Fire, ESM::Light::Pulse })
            {
                EXPECT_TRUE(castsWherePlaced(makeRecord(100, 0x00FFFFFF, flag))) << "flag " << flag;
                EXPECT_TRUE(makeLight(makeRecord(100, 0x00FFFFFF, flag), osg::Vec3f()).has_value()) << "flag " << flag;
            }

            // A file on disk that something else wrote, so a radius of nothing is data and not a
            // broken contract.
            EXPECT_FALSE(makeLight(makeRecord(0, 0x00FFFFFF, 0), osg::Vec3f()).has_value());
            EXPECT_FALSE(makeLight(makeRecord(-50, 0x00FFFFFF, 0), osg::Vec3f()).has_value());
        }

        /// A light that subtracts is refused by both routes to one, and the graph was the half that
        /// was wrong.
        ///
        /// **`SceneUtil::createLightSource` has no notion of "not a light".** It answers a `Negative`
        /// record by negating the diffuse and handing back a `LightSource` like any other, so the
        /// walk mirrored a lamp of negative intensity exactly where the harness placed none. The
        /// refusal now lives where a colour and a radius meet, which is the one place both routes
        /// pass through — and this builds the graph the game builds rather than a negative colour by
        /// hand, so it is the real path that is refused.
        TEST(RtxLightBuilderTest, aLightThatSubtractsIsRefusedByBothRoutesToOne)
        {
            const ESM::Light subtracting = makeRecord(100, 0x00FFFFFF, ESM::Light::Negative);

            const osg::ref_ptr<SceneUtil::LightSource> built = SceneUtil::createLightSource(
                SceneUtil::LightCommon(subtracting), SceneUtil::Mask_Lighting, /*isExterior=*/false);
            ASSERT_NE(built, nullptr);

            const osg::Vec3f radiated = lightColour(*built, 0.0);
            ASSERT_LT(radiated.x(), 0.0f) << "the graph did not build a light that subtracts, so this proves nothing";

            EXPECT_FALSE(makeLight(radiated, 100.0f, osg::Vec3f()).has_value()) << "the walk mirrored it anyway";
            EXPECT_FALSE(makeLight(subtracting, osg::Vec3f()).has_value()) << "and the record it was built from";

            // The same record without the flag is an ordinary white lamp by both routes, so what the
            // two agree on is the flag and not the light.
            const ESM::Light ordinary = makeRecord(100, 0x00FFFFFF, 0);
            const osg::ref_ptr<SceneUtil::LightSource> lit = SceneUtil::createLightSource(
                SceneUtil::LightCommon(ordinary), SceneUtil::Mask_Lighting, /*isExterior=*/false);

            EXPECT_TRUE(makeLight(lightColour(*lit, 0.0), 100.0f, osg::Vec3f()).has_value());
            EXPECT_TRUE(makeLight(ordinary, osg::Vec3f()).has_value());

            // **A black record subtracts nothing, so the flag on it decides nothing either.** Both
            // routes place a lamp that radiates zero, which is what they already did for a black
            // record without the flag — the record path used to drop this one and disagree.
            const ESM::Light unlit = makeRecord(100, 0x00000000, ESM::Light::Negative);
            const osg::ref_ptr<SceneUtil::LightSource> dark = SceneUtil::createLightSource(
                SceneUtil::LightCommon(unlit), SceneUtil::Mask_Lighting, /*isExterior=*/false);

            EXPECT_TRUE(makeLight(lightColour(*dark, 0.0), 100.0f, osg::Vec3f()).has_value());
            EXPECT_TRUE(makeLight(unlit, osg::Vec3f()).has_value());
        }
    }
}
