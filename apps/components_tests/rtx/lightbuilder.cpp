#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include <osg/PositionAttitudeTransform>

#include <components/esm3/loadligh.hpp>
#include <components/rtx/lightbuilder.hpp>
#include <components/sceneutil/lightcommon.hpp>
#include <components/sceneutil/lightcontroller.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/lightutil.hpp>
#include <components/sceneutil/util.hpp>
#include <components/sceneutil/vismask.hpp>

#include "graphlight.hpp"
#include "statistics.hpp"

namespace Rtx
{
    namespace
    {
        /// A `LIGH` record reduced the way the engine reduces one, which is what every rule about a
        /// light reads. The flags are `ESM::Light`'s, because that is what the file carries.
        SceneUtil::LightCommon describe(std::int32_t radius, std::uint32_t colour, std::int32_t flags)
        {
            ESM::Light record;
            record.mData.mRadius = radius;
            record.mData.mColor = colour;
            record.mData.mFlags = flags;
            return SceneUtil::LightCommon(record);
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
                EXPECT_NEAR(Testing::meanOf(run), 1.0f, 0.001f);

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

            const osg::ref_ptr<SceneUtil::LightSource> full = Testing::makeLightSource(0.0f, grey, glow);
            const osg::ref_ptr<SceneUtil::LightSource> half = Testing::makeLightSource(0.0f, grey, glow);
            half->setActorFade(0.5f);

            EXPECT_NEAR(lightColour(*half, 0.0).x(), lightColour(*full, 0.0).x() * 0.5f, 1e-5f);

            // What the distance fade reaches exactly at `actors processing range`, which is the
            // frame before the node mask takes the whole actor out of the picture.
            const osg::ref_ptr<SceneUtil::LightSource> gone = Testing::makeLightSource(0.0f, grey, glow);
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
            const SceneUtil::LightCommon record = describe(100, 0x00FFFFFF, ESM::Light::PulseSlow);

            const osg::ref_ptr<SceneUtil::LightSource> lamp = SceneUtil::createLightSource(
                record, SceneUtil::Mask_Lighting, /*isExterior=*/false, osg::Vec4f(1, 1, 1, 1));

            // A pulse turns once in three seconds. Eight samples across it put one of them within an
            // eighth of a turn of the peak, so the deepest is at least `0.35 * cos(pi / 8)` from
            // rest — and every one of them carries the same ambient, which is the point.
            const osg::Vec3f white
                = lightColour(*Testing::makeLightSource(0.0f, osg::Vec4f(), osg::Vec4f(1, 1, 1, 1)), 0.0);

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
            EXPECT_NEAR(lightColour(*Testing::makeLightSource(0.0f, grey, osg::Vec4f()), 0.0).x(), 0.21586f, 1e-5f);

            // What `Animation::setLightEffect` builds: nothing in the diffuse, 1.5 in the ambient.
            // ((1.5 + 0.055) / 1.055)^2.4 = 2.53716, and a walk reading the diffuse alone gets zero.
            const osg::Vec3f glow = lightColour(
                *Testing::makeLightSource(0.0f, osg::Vec4f(0, 0, 0, 0), osg::Vec4f(1.5f, 1.5f, 1.5f, 1)), 0.0);
            EXPECT_NEAR(glow.x(), 2.53716f, 1e-4f);
            EXPECT_NEAR(glow.z(), 2.53716f, 1e-4f);

            // Both at once add as light adds, after each is decoded and not before: 0.21586 of red
            // on top of 2.53716 of white.
            const osg::Vec3f both
                = lightColour(*Testing::makeLightSource(0.0f, grey, osg::Vec4f(1.5f, 1.5f, 1.5f, 1)), 0.0);
            EXPECT_NEAR(both.x(), 2.75302f, 1e-4f);
            EXPECT_NEAR(both.y(), 2.53716f, 1e-4f);

            // **The property the whole function exists for.** The harness reads a cell's `LIGH`
            // records and the game reads the `SceneUtil::LightSource` nodes its graph already holds;
            // for one record those two have to be one light, down to the last bit of the intensity.
            for (const std::uint32_t packed : { 0x00000000u, 0x00808080u, 0x000080FFu, 0x00FFFFFFu })
            {
                const SceneUtil::LightCommon record = describe(100, packed, 0);
                const std::optional<Rtx::Light> fromRecord = makeLight(record, osg::Vec3f(1, 2, 3));

                const osg::ref_ptr<SceneUtil::LightSource> graph
                    = Testing::makeLightSource(0.0f, SceneUtil::colourFromRGB(packed), osg::Vec4f());
                const std::optional<Rtx::Light> fromGraph
                    = makeLight(lightColour(*graph, 0.0), 100.0f, osg::Vec3f(1, 2, 3));

                ASSERT_TRUE(fromRecord.has_value() && fromGraph.has_value()) << "packed " << packed;
                EXPECT_EQ(fromRecord->mIntensity, fromGraph->mIntensity) << "packed " << packed;
                EXPECT_EQ(fromRecord->mReach, fromGraph->mReach);
                EXPECT_EQ(fromRecord->mSourceRadius, fromGraph->mSourceRadius);
                EXPECT_EQ(fromRecord->mClearance, fromGraph->mClearance);
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
            const std::optional<Rtx::Light> light = makeLight(describe(100, 0x00FFFFFF, 0), osg::Vec3f(1, 2, 3));

            ASSERT_TRUE(light.has_value());
            EXPECT_EQ(light->mPosition, osg::Vec3f(1, 2, 3));

            // 100 * 100 * 0.25 * pi = 7853.98, and white decodes to one.
            EXPECT_NEAR(light->mIntensity.x(), 7853.98f, 0.01f);
            EXPECT_NEAR(light->mIntensity.y(), 7853.98f, 0.01f);

            // 100 * 2 + 128.
            EXPECT_FLOAT_EQ(light->mReach, 328.0f);

            // A sixteenth of the record: 6.25 units, which is nine centimetres across at seventy
            // units to the metre — a flame, and not the metre and a half the reach describes. The
            // fitting around it is a quarter of the record, which is what the ray keeps clear of.
            EXPECT_FLOAT_EQ(light->mSourceRadius, 6.25f);
            EXPECT_FLOAT_EQ(light->mClearance, 25.0f);

            // Doubling the radius quadruples the brightness, doubles the flame and rather less than
            // doubles the reach: 200 * 200 * 0.25 * pi = 31415.9, 200 / 16 = 12.5, and
            // 200 * 2 + 128 = 528.
            const std::optional<Rtx::Light> larger = makeLight(describe(200, 0x00FFFFFF, 0), osg::Vec3f());
            ASSERT_TRUE(larger.has_value());
            EXPECT_NEAR(larger->mIntensity.x(), 31415.9f, 0.1f);
            EXPECT_FLOAT_EQ(larger->mReach, 528.0f);
            EXPECT_FLOAT_EQ(larger->mSourceRadius, 12.5f);
            EXPECT_FLOAT_EQ(larger->mClearance, 50.0f);

            // The two readings of the record are one reading: an emitter of fixed radiance is
            // brighter by its area, so the brightness has to be the square of the size for a candle
            // and a brazier to be the same fire at two scales rather than two arbitrary lamps.
            EXPECT_NEAR(larger->mIntensity.x() / light->mIntensity.x(),
                (larger->mSourceRadius / light->mSourceRadius) * (larger->mSourceRadius / light->mSourceRadius), 1e-4f);
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
            EXPECT_FALSE(castsWherePlaced(describe(100, 0x00FFFFFF, ESM::Light::OffDefault)));
            EXPECT_FALSE(makeLight(describe(100, 0x00FFFFFF, ESM::Light::OffDefault), osg::Vec3f()).has_value());

            EXPECT_FALSE(makeLight(describe(100, 0x00FFFFFF, ESM::Light::Negative), osg::Vec3f()).has_value());

            // The flags that say what a light is or how it animates leave it burning.
            for (const std::int32_t flag :
                { ESM::Light::Carry, ESM::Light::Dynamic, ESM::Light::Flicker, ESM::Light::Fire, ESM::Light::Pulse })
            {
                EXPECT_TRUE(castsWherePlaced(describe(100, 0x00FFFFFF, flag))) << "flag " << flag;
                EXPECT_TRUE(makeLight(describe(100, 0x00FFFFFF, flag), osg::Vec3f()).has_value()) << "flag " << flag;
            }

            // A file on disk that something else wrote, so a radius of nothing is data and not a
            // broken contract.
            EXPECT_FALSE(makeLight(describe(0, 0x00FFFFFF, 0), osg::Vec3f()).has_value());
            EXPECT_FALSE(makeLight(describe(-50, 0x00FFFFFF, 0), osg::Vec3f()).has_value());
        }

        /// A lamp stands at its wick, and a lamp with no mesh stands where the reference does.
        ///
        /// **The two callers of `standLight` want opposite halves of one rule.** A cell the player
        /// has walked into places the model, so the light belongs on the `AttachLight` node its
        /// author put at the flame — up to forty-eight units above the reference on the lamps of one
        /// Balmora room. The reach around that cell places no model at all, because
        /// `Terrain::pagedType` stands none, so the light lands on the transform the reference's own
        /// position built. `SceneUtil::addLight` is what tells the two apart, and this is what says
        /// `standLight` reaches it rather than `createLightSource`.
        TEST(RtxLightBuilderTest, aLampStandsAtItsWickAndOneWithNoMeshStandsWhereItLies)
        {
            const SceneUtil::LightCommon burning = describe(100, 0x00FFFFFF, ESM::Light::Flicker);

            // A model as a loader hands one over: the mesh under a transform, and the flame's own
            // node named where the author put it.
            osg::ref_ptr<osg::Group> lantern = new osg::Group;
            osg::ref_ptr<osg::PositionAttitudeTransform> wick = new osg::PositionAttitudeTransform;
            wick->setName("AttachLight");
            wick->setPosition(osg::Vec3f(0.0f, 0.0f, 48.0f));
            lantern->addChild(wick);

            ASSERT_TRUE(standLight(*lantern, burning, /*exterior=*/false));
            ASSERT_EQ(wick->getNumChildren(), 1u) << "the light was not hung on the flame's own node";
            EXPECT_EQ(lantern->getNumChildren(), 1u) << "the light was hung on the model as well";

            // The reach around a cell, which reads the record and never the mesh.
            osg::ref_ptr<osg::Group> bare = new osg::Group;
            ASSERT_TRUE(standLight(*bare, burning, /*exterior=*/true));
            ASSERT_EQ(bare->getNumChildren(), 1u);

            // **Marked the way the game marks one**, because the two graphs have to look the same to
            // anything that ever filters on it.
            EXPECT_EQ(bare->getChild(0)->getNodeMask(), SceneUtil::Mask_Lighting);
            EXPECT_EQ(wick->getChild(0)->getNodeMask(), SceneUtil::Mask_Lighting);

            // And a record that does not burn stands nothing on either path.
            const SceneUtil::LightCommon unlit = describe(100, 0x00FFFFFF, ESM::Light::OffDefault);
            osg::ref_ptr<osg::Group> dark = new osg::Group;
            EXPECT_FALSE(standLight(*dark, unlit, /*exterior=*/false));
            EXPECT_EQ(dark->getNumChildren(), 0u);
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
            const SceneUtil::LightCommon subtracting = describe(100, 0x00FFFFFF, ESM::Light::Negative);

            const osg::ref_ptr<SceneUtil::LightSource> built
                = SceneUtil::createLightSource(subtracting, SceneUtil::Mask_Lighting, /*isExterior=*/false);
            ASSERT_NE(built, nullptr);

            const osg::Vec3f radiated = lightColour(*built, 0.0);
            ASSERT_LT(radiated.x(), 0.0f) << "the graph did not build a light that subtracts, so this proves nothing";

            EXPECT_FALSE(makeLight(radiated, 100.0f, osg::Vec3f()).has_value()) << "the walk mirrored it anyway";
            EXPECT_FALSE(makeLight(subtracting, osg::Vec3f()).has_value()) << "and the record it was built from";

            // The same record without the flag is an ordinary white lamp by both routes, so what the
            // two agree on is the flag and not the light.
            const SceneUtil::LightCommon ordinary = describe(100, 0x00FFFFFF, 0);
            const osg::ref_ptr<SceneUtil::LightSource> lit
                = SceneUtil::createLightSource(ordinary, SceneUtil::Mask_Lighting, /*isExterior=*/false);

            EXPECT_TRUE(makeLight(lightColour(*lit, 0.0), 100.0f, osg::Vec3f()).has_value());
            EXPECT_TRUE(makeLight(ordinary, osg::Vec3f()).has_value());

            // **A black record subtracts nothing, so the flag on it decides nothing either.** Both
            // routes place a lamp that radiates zero, which is what they already did for a black
            // record without the flag — the record path used to drop this one and disagree.
            const SceneUtil::LightCommon unlit = describe(100, 0x00000000, ESM::Light::Negative);
            const osg::ref_ptr<SceneUtil::LightSource> dark
                = SceneUtil::createLightSource(unlit, SceneUtil::Mask_Lighting, /*isExterior=*/false);

            EXPECT_TRUE(makeLight(lightColour(*dark, 0.0), 100.0f, osg::Vec3f()).has_value());
            EXPECT_TRUE(makeLight(unlit, osg::Vec3f()).has_value());
        }
    }
}
