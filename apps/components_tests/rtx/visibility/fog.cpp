#include <components/rtx/shaders/look.h>

#include "fixture.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace Rtx::Testing
{
    namespace
    {
        /// What colour the air is. Named for the reason `sFoggySky` is: each expectation computes
        /// with it as well as handing it to the shader. Deliberately not grey, so a fog scattering
        /// the wrong colour cannot pass by matching a total.
        const osg::Vec3f sHaze(0.1f, 0.2f, 0.4f);

        /// A coverage band faint enough that the air is still even enough to compute with.
        ///
        /// **One path serves every kind of air**, so this chooses nothing but how much of the band
        /// the arithmetic carries. At a thousandth it moves the density by at most two parts in a
        /// thousand, which is below anything asserted here, so a test that wants the band exercised
        /// sets this and one that does not sets one. The banks have their own tests, and those run
        /// at nought.
        constexpr float sVolumeOverEvenAir = 0.999f;

        /// What one unit of a lamp's intensity delivers `span` units away, from the same windowed
        /// inverse square the shader uses: an inverse square that reaches exactly zero at the
        /// lamp's reach, because Morrowind's is a hard cutoff and clipping one leaves a ring.
        constexpr float lampDelivered(float span, float reach)
        {
            const float ratio = span / reach;
            const float window = 1.0f - ratio * ratio * ratio * ratio;
            return window * window / (span * span + 1.0f);
        }

        /// A level rectangle standing between a ray along the y axis and a lamp above it.
        ///
        /// **Not `sheetAt`, because half a lid is the point.** A square about the origin is
        /// either over the whole march or over none of it, and what these tests need is a shadow
        /// boundary that falls where they put it rather than where the grid does.
        std::array<osg::Vec3f, 4> makeLid(float height, float from, float to, float halfWidth)
        {
            return {
                osg::Vec3f(-halfWidth, from, height),
                osg::Vec3f(halfWidth, from, height),
                osg::Vec3f(halfWidth, to, height),
                osg::Vec3f(-halfWidth, to, height),
            };
        }

        /// Puts `camera` under a sky of one radiance and nothing else, with air of `extinction` in it
        /// pooling at `level`.
        void litThroughFog(Shaders::VisibilityConstants& camera, float extinction,
            float level = -std::numeric_limits<float>::infinity())
        {
            camera.mSkyHorizon = osg::Vec3f(sFoggySky, sFoggySky, sFoggySky);
            camera.mSkyZenith = camera.mSkyHorizon;
            camera.mAmbientFromSky = 1.0f;
            camera.mFogColour = sHaze;
            camera.mFogExtinction = extinction;
            camera.mWaterLevel = level;

            // Even air, which is what every exact expectation here needs: a banked field varies
            // along the ray, and its answer then stops being one anyone can write down. The banks
            // have their own test.
            camera.mFogUniform = 1.0f;
        }

        /// Fog takes what Beer-Lambert says over the path and gives its own colour back in place.
        ///
        /// **A horizontal ray is what makes the march exact.** The layer's density varies only with
        /// height, so along a ray that holds its own the medium is uniform — and then the march
        /// telescopes: the per-step transmittances multiply to `exp(-sigma * span)` whatever the
        /// step distribution is, and the scattered terms sum to `colour * (1 - T)`. So this asserts
        /// the analytic answer rather than the march's approximation of it.
        TEST_F(RtxVisibilityTest, fogTakesWhatBeerLambertSaysOverThePathAndGivesItsOwnColourBack)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);
            constexpr float distance = 2000.0f;
            constexpr float extinction = 3.5e-4f;

            const auto look = [&](float thickness) {
                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -distance, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
                litThroughFog(camera, thickness);

                const SceneDesc scene = makeWall();
                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels);
                return std::array<int, 3>{ pixels[centre], pixels[centre + 1], pixels[centre + 2] };
            };

            // The wall is untextured, so its albedo is 0.5 and the cell's ambient is all that is on
            // it: 0.5 * 0.6 = 0.3. Over that sits `exp(-3.5e-4 * 2000)` = 0.4966 of transmittance,
            // with the rest of the path's worth of the fog's own colour in front of it.
            const float transmittance = std::exp(-extinction * distance);
            const std::array<int, 3> fogged = look(extinction);
            constexpr float wall = 0.5f * sFoggySky;
            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                const auto expected
                    = static_cast<int>(encodeSrgb(wall * transmittance + sHaze[channel] * (1.0f - transmittance)));

                EXPECT_NEAR(fogged[channel], expected, 1) << "channel " << channel;
            }

            // The three channels also have to come apart the way the fog's colour does, or a grey
            // haze would pass the arithmetic above by accident.
            EXPECT_LT(fogged[0], fogged[1]) << "the fog's own colour, showing through";
            EXPECT_LT(fogged[1], fogged[2]);

            // And no fog is *exactly* no fog. A lit surface with air over it is a differently lit
            // one, which is why every test here that measures radiance leaves this at zero.
            const std::array<int, 3> clear = look(0.0f);
            for (const int level : clear)
                EXPECT_EQ(level, int{ encodeSrgb(wall) }) << "with no fog in the cell";
        }

        /// The fog layer sits on the water, thins with height above it, and stops at the surface.
        ///
        /// Fog gathers over water and drains off high ground, so the level a cell records is where
        /// its layer sits — and *at* it rather than in it, because a point under the surface already
        /// has the water's own absorption over it. Fog there would be a second medium laid on the
        /// first, putting grey between the eye and the seabed twice over.
        TEST_F(RtxVisibilityTest, theFogLayerSitsOnTheWaterThinsAboveItAndStopsAtIt)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);
            constexpr float distance = 2000.0f;
            constexpr float extinction = 3.5e-4f;

            // The ray runs level at z = 0, so how much fog it crosses is decided by where the layer
            // is put under it and by nothing else.
            const auto look = [&](float level, float thickness) {
                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -distance, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
                litThroughFog(camera, thickness, level);

                const SceneDesc scene = makeWall();
                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels);
                return std::array<int, 3>{ pixels[centre], pixels[centre + 1], pixels[centre + 2] };
            };

            // A dry cell is handed minus infinity and falls back to sea level, which is where a
            // water level of zero puts the layer anyway — so the two hold the same air above the
            // base and have to draw the same frame through it.
            //
            // **Above the base and not on it.** The two are different worlds *below* it: a dry
            // cell's layer is capped at its full strength down there and a flooded one holds no air
            // at all, since the water's own absorption is what stands over a point under the
            // surface. A ray along the base samples both sides of that step — a column of the
            // volume has width, and the tent reaches a column either side of it, which at this
            // frame and this distance is some five hundred units of height at the far end — so the
            // two read up to thirteen levels apart there and agree exactly six hundred units over
            // it. The closed form this replaced integrated the profile along the ray alone and so
            // could not see the step at all.
            constexpr float clearOfTheBase = 600.0f;
            const auto lookAbove = [&](float level) {
                Shaders::VisibilityConstants camera = makeCamera(osg::Vec3f(0.0f, -distance, clearOfTheBase),
                    osg::Vec3f(0.0f, 0.0f, clearOfTheBase), 60.0f, size, size, 100000.0f);
                litThroughFog(camera, extinction, level);

                // Stretched, because the ray runs six hundred units over the middle of the wall the
                // rest of this test is measured against.
                const SceneDesc scene = makeWall(20.0f);
                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels);
                return std::array<int, 3>{ pixels[centre], pixels[centre + 1], pixels[centre + 2] };
            };

            const std::array<int, 3> dry = lookAbove(-std::numeric_limits<float>::infinity());
            const std::array<int, 3> atSeaLevel = lookAbove(0.0f);
            for (std::size_t channel = 0; channel < 3; ++channel)
                EXPECT_NEAR(dry[channel], atSeaLevel[channel], 1) << "channel " << channel << ", the dry-cell fallback";

            // Three thousand units under the ray, so it runs `exp(-3000 / 2600)` = 0.3154 of the way
            // up the layer's own falloff and crosses less than a third of the fog.
            constexpr float below = 3000.0f;
            const float thinned = extinction * std::exp(-below / Shaders::FOG_HEIGHT);
            const float transmittance = std::exp(-thinned * distance);

            const std::array<int, 3> high = look(-below, extinction);
            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                const auto expected = static_cast<int>(
                    encodeSrgb(0.5f * sFoggySky * transmittance + sHaze[channel] * (1.0f - transmittance)));

                EXPECT_NEAR(high[channel], expected, 1) << "channel " << channel << ", high over the layer";
            }

            // And with the surface over the ray instead there is no air to fog at all: the frame is
            // whatever the water did to it and nothing else, whatever the cell's fog says.
            const std::array<int, 3> submerged = look(100.0f, extinction);
            const std::array<int, 3> withoutFog = look(100.0f, 0.0f);
            for (std::size_t channel = 0; channel < 3; ++channel)
                EXPECT_EQ(submerged[channel], withoutFog[channel]) << "channel " << channel << ", under the surface";

            // **The falloff itself, which no level ray crosses.** Every expectation above holds the
            // ray at one height, so the layer is one number along it; a ray that climbs out of the
            // layer crosses the whole curve, and what it is charged for is its own length times the
            // mean of `exp(-z / FOG_HEIGHT)` over the heights it passed.
            constexpr float climb = 2000.0f;
            const auto lookSloping = [&](const osg::Vec3f& eye, const osg::Vec3f& target) {
                Shaders::VisibilityConstants camera = makeCamera(eye, target, 60.0f, size, size, 100000.0f);
                litThroughFog(camera, extinction);

                // Stretched, because a ray leaving at forty-five degrees meets the wall two thousand
                // units off its middle and the wall is four hundred across.
                const SceneDesc scene = makeWall(20.0f);
                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels);
                return std::array<int, 3>{ pixels[centre], pixels[centre + 1], pixels[centre + 2] };
            };

            // Two thousand up over two thousand along is a path of 2828.43, and the layer's mean
            // over the first 2000/2600 of its falloff is (1 - exp(-0.76923)) / 0.76923 = 0.69762 —
            // so 1973.1 units' worth of air at the base's own density, and `exp(-3.5e-4 * 1973.1)`
            // is 0.50125 of transmittance.
            const float slope = std::sqrt(distance * distance + climb * climb);
            const float crossed = climb / Shaders::FOG_HEIGHT;
            const float climbing = std::exp(-extinction * slope * (1.0f - std::exp(-crossed)) / crossed);

            const std::array<int, 3> up = lookSloping(osg::Vec3f(0.0f, -distance, 0.0f), osg::Vec3f(0.0f, 0.0f, climb));
            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                const auto expected
                    = static_cast<int>(encodeSrgb(0.5f * sFoggySky * climbing + sHaze[channel] * (1.0f - climbing)));

                EXPECT_NEAR(up[channel], expected, 1) << "channel " << channel << ", climbing out of the layer";
            }

            // And a ray descending the same two heights crosses the same air, which is the other
            // half of the integral and the one a sign error would have shown up in. Within a level:
            // the volume samples each slice at a jittered point rather than integrating the curve,
            // and the two rays cross the slices at different heights.
            const std::array<int, 3> down
                = lookSloping(osg::Vec3f(0.0f, -distance, climb), osg::Vec3f(0.0f, 0.0f, 0.0f));
            for (std::size_t channel = 0; channel < 3; ++channel)
                EXPECT_NEAR(down[channel], up[channel], 1) << "channel " << channel << ", the same heights descending";
        }

        /// An eye under the surface has no air in front of it, and the volume must say so too.
        ///
        /// **The one path that could not tell on its own.** `fogExtinctionAt` gives nothing under the
        /// surface and `fogColumn` integrates nothing there, so the field and the closed form were
        /// right already. The volume is an accumulation along a *column's* ray rather than a field
        /// read along the pixel's, and a froxel the surface stands inside draws its sample from the
        /// air the column found — which for a column aimed up out of the water is the air above it.
        /// A pixel under the water then paid for that air, in a band along the waterline where the
        /// columns first begin to leave the water: 135 pixels of this frame, the worst of them 61 of
        /// 255 out.
        ///
        /// **Asserted as the whole frame against the same frame with no weather in the cell**, which
        /// is the only exact form the claim has: nothing else differs between the two, so a submerged
        /// eye owes the fog literally nothing and every byte has to match.
        TEST_F(RtxVisibilityTest, theVolumesAirIsNotOverAnEyeUnderTheSurface)
        {
            constexpr std::uint32_t size = 65;

            // Level at fifty units down, so the frame holds the surface overhead, the bed below and
            // the waterline between them — which is where the band was.
            const SceneDesc scene = makeFlooded(8000.0f, 400.0f);

            const auto look = [&](float extinction) {
                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -1000.0f, -50.0f), osg::Vec3f(0.0f, 0.0f, -50.0f), 60.0f, size, size, 100000.0f);
                litThroughFog(camera, extinction, 0.0f);
                camera.mFogUniform = sVolumeOverEvenAir;

                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels, { .mSea = SeaState{ .mSignificantHeight = 0.0f } });
                return pixels;
            };

            const std::vector<std::uint8_t> foggy = look(3.5e-4f);
            const std::vector<std::uint8_t> clear = look(0.0f);

            ASSERT_EQ(foggy.size(), clear.size());
            for (std::uint32_t row = 0; row < size; ++row)
                for (std::uint32_t column = 0; column < size; ++column)
                {
                    const std::size_t at = (std::size_t{ row } * size + column) * 4;
                    ASSERT_EQ(foggy[at], clear[at]) << "row " << row << ", column " << column;
                }
        }

        /// A lamp lights the air it stands in, and by the isotropic share of what it delivers.
        ///
        /// **`INV_FOUR_PI` is what this is really about.** A lamp reaches a point in the fog as
        /// irradiance, exactly as it reaches a surface, and what comes back toward the eye is that
        /// irradiance spread over the whole sphere. Dropping the factor is not subtle — it lights
        /// the air 4pi times over, and the centre pixel here goes from 121 of 255 to 211.
        ///
        /// **The lamp is put far enough away that its falloff is flat along the ray**, which is what
        /// makes the march's answer analytic: with the light it delivers constant, the scattered
        /// terms telescope to `E / 4pi * (1 - T)` exactly as a constant fog colour does. Twenty
        /// thousand units off a two-hundred-unit ray varies the irradiance by 0.02%.
        TEST_F(RtxVisibilityTest, aLampLightsTheAirItStandsInByTheIsotropicShareOfWhatItDelivers)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);
            constexpr float distance = 200.0f;
            constexpr float reach = 30000.0f;

            // Half the ray's worth of fog, so the wall and the air contribute comparably.
            const float extinction = std::log(2.0f) / distance;

            // Behind the wall in y, so its cosine there is negative and the lamp lights the air
            // without also lighting what the air is in front of.
            const osg::Vec3f lamp(0.0f, 100.0f, 20000.0f);

            const osg::Vec3f middle(0.0f, -0.5f * distance, 0.0f);
            const float delivered = lampDelivered((lamp - middle).length(), reach);

            const auto look = [&](bool lit, bool shaded = false) {
                SceneDesc scene = makeWall();

                // A lid between the ray and the lamp, high enough to be nowhere near what the eye
                // sees and squarely across every ray the march sends up at the light.
                if (shaded)
                    scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                        .mMesh = scene.addMesh(sheetAt(40000.0f, 1000.0f), {}, {}, sQuadIndices) });

                if (lit)
                    scene.addLight(Light{
                        .mPosition = lamp,
                        // Scaled so the lamp delivers exactly one unit of irradiance to the ray,
                        // which is what lets the expectation below be written without it.
                        .mIntensity = osg::Vec3f(1.0f, 1.0f, 1.0f) / delivered,
                        .mReach = reach,
                    });

                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -distance, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
                litThroughFog(camera, extinction);

                // Black air, so the lamp is the only thing in the frame the fog scatters and the
                // expectation below needs no term for the haze `litThroughFog` would otherwise put
                // in it.
                camera.mFogColour = osg::Vec3f();

                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels);
                return int{ pixels[centre] };
            };

            // With no lamp the air scatters nothing, because the fog's own colour is black here: the
            // wall is all there is, at half of it.
            constexpr float wall = 0.5f * sFoggySky;
            EXPECT_EQ(look(false), int{ encodeSrgb(0.5f * wall) }) << "black air over a lit wall";

            // And with it, one unit of irradiance through an isotropic sphere, over the half of the
            // ray's light the fog took: 0.15 + 1 / (4 pi) * 0.5 = 0.18979, which encodes to 121.
            const float scattered = 0.25f * Shaders::INV_PI * 0.5f;
            EXPECT_NEAR(look(true), int{ encodeSrgb(0.5f * wall + scattered) }, 1) << "the lamp in the air";

            // **And nothing at all through a lid**, which is the whole of what the march's one ray
            // buys: every lamp at every step is weighed into one reservoir and the one held is
            // traced to, so a lantern behind something stops lighting the air in front of it.
            EXPECT_EQ(look(true, true), look(false)) << "a lamp behind a lid still lit the air";
        }

        /// Where the volume's own air stands, how thick it is, and what lights it.
        ///
        /// **One fixture for the two tests below, because they differ in the lid and in nothing
        /// else.** Both stand a lamp beside a two thousand unit ray with a reach that covers eight
        /// hundred units of it, which is less than the stretch one probe used to answer for — and
        /// that is the whole of what they are about. A lamp reaching the whole ray asks the volume
        /// no question the closed form has not already answered.
        ///
        /// **A narrow field of view, because a column is eight pixels wide.** At sixty degrees a
        /// column of a thirty-three pixel frame spans thirteen of them, so the ray the middle pixel
        /// reads its air along leaves the ray it was traced along by hundreds of units. Ten degrees
        /// puts that under forty, which is small against a reach of five hundred.
        struct LampInTheAir
        {
            /// How far the wall stands, and so how long the ray the middle pixel reads is.
            static constexpr float sDistance = 2000.0f;

            /// Half the ray's light, which leaves the wall and the air comparable.
            static constexpr float sExtinction = 0.693147f / sDistance;

            /// Where the lamp stands and how far it carries. Three hundred units up off the ray and
            /// a reach of five hundred, so it lights the stretch of it between 385 and 1215.
            static inline const osg::Vec3f sLamp{ 0.0f, -1200.0f, 300.0f };
            static constexpr float sReach = 512.0f;

            /// What it delivers where the ray passes closest, which the intensity is scaled to
            /// rather than named — so the expectations below need no falloff in them.
            static constexpr float sIrradiance = 12.0f;
            static inline const osg::Vec3f sIntensity
                = osg::Vec3f(1.0f, 1.0f, 1.0f) * (sIrradiance / lampDelivered(sLamp.z(), sReach));

            /// How high the lid hangs. Between the ray and the lamp, so a shadow ray from a point
            /// on the ray crosses it half way along.
            static constexpr float sLidHeight = 150.0f;
        };

        /// A lamp behind a lid lights none of the volume's air either.
        ///
        /// **The same assertion as the one above and not the same test.** The closed form weighs
        /// every lamp of the whole ray into one reservoir and buys one ray with it, so a lid over
        /// the march takes the whole lamp with it. The volume answers a froxel at a time, and the
        /// question here is whether the *seeing* it charges a froxel is the seeing at that froxel.
        ///
        /// **A short reach is what makes the question sharp.** A probe drawn anywhere along a
        /// stretch and outside the lamp's reach finds no lamp, so it holds nothing, buys no ray,
        /// and `lampVisible` answers one for want of anything to trace — while every slice of the
        /// stretch still sums the lamp where it actually stands. A lantern behind a lid then lights
        /// the air in front of it, which is what this refuses.
        ///
        /// **The lid stands in both frames**, so the two scenes differ in the lamp and in nothing
        /// else: a lid also covers part of the sky the wall gathers, and a test that added one
        /// would be measuring that as well.
        TEST_F(RtxVisibilityTest, aLampBehindALidLightsNoneOfTheVolumesAirEither)
        {
            using Fixture = LampInTheAir;

            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);

            const auto look = [&](bool lit, bool lidded) {
                SceneDesc scene = makeWall();

                if (lidded)
                    scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                        .mMesh = scene.addMesh(
                            makeLid(Fixture::sLidHeight, -4000.0f, 4000.0f, 4000.0f), {}, {}, sQuadIndices) });

                if (lit)
                    scene.addLight(Light{
                        .mPosition = Fixture::sLamp,
                        .mIntensity = Fixture::sIntensity,
                        .mReach = Fixture::sReach,
                    });

                Shaders::VisibilityConstants camera = makeCamera(osg::Vec3f(0.0f, -Fixture::sDistance, 0.0f),
                    osg::Vec3f(0.0f, 0.0f, 0.0f), 10.0f, size, size, 100000.0f);
                litThroughFog(camera, Fixture::sExtinction);
                camera.mFogUniform = sVolumeOverEvenAir;

                // Black air, so the lamp is the only thing the fog has to scatter and the two
                // frames below can be compared as they stand.
                camera.mFogColour = osg::Vec3f();

                // **Averaged, because one frame is one draw of the probe.** What is asserted is
                // where the leak sits on average; a single frame either leaked or did not, and
                // which is a coin.
                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels, { .mFrames = 32 });
                return int{ pixels[centre] };
            };

            const int dark = look(false, true);
            const int lidded = look(true, true);
            const int open = look(true, false);

            EXPECT_EQ(lidded, dark) << "the lid takes the whole lamp out of the air, not most of it";
            EXPECT_GT(open, dark + 20) << "and there was a lamp in the air to take";
        }

        /// The air under a lamp settles instead of flickering block by block.
        ///
        /// **What a boiling image is, measured as what it is.** A froxel stands for eight pixels
        /// squared, so an estimator that decides one thing for a whole stretch of a column paints
        /// that decision across a block of the frame — and redecides it next frame. The complaint
        /// is not that the mean is wrong, it is that the frames do not stand still, so what this
        /// measures is how far a settled pixel moves from one frame to the next.
        ///
        /// **The step and not the spread.** The volume averages nine tenths of its history in, so
        /// consecutive frames of a run are strongly correlated and a standard deviation over a
        /// short run is mostly an estimate of that correlation. The mean step between neighbours is
        /// what a viewer sees, and its own estimator settles in a run this length.
        ///
        /// **Half the lit stretch under a lid**, so the seeing genuinely changes along the ray:
        /// with none of it shadowed there is nothing for a probe to be wrong about, and with all of
        /// it shadowed the test above already asks the question.
        ///
        /// **And the bound is a bound and not the figure.** This settles at 0.4% of the pixel's own
        /// value, where the estimator it replaced — one probe answering for a stretch of eight
        /// slices — swings by about half the lamp's whole term every frame, which the history takes
        /// to a tenth of the pixel. Three per cent stands between the two with room either side.
        ///
        /// The same estimator with the history switched off measures 5.9%, which is what the
        /// temporal half of the filter is worth here and why a froxel reprojects its own middle.
        TEST_F(RtxVisibilityTest, theVolumeSettlesTheAirUnderALampRatherThanFlickeringBlockByBlock)
        {
            using Fixture = LampInTheAir;

            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreOf(size);

            // Long enough that the history is settled well before the window, and that the window
            // holds many of the ten-frame spans the history's own weight correlates over.
            constexpr std::size_t frames = 192;
            constexpr std::size_t settled = 64;

            SceneDesc scene = makeWall();

            // Over the near half of the stretch the lamp reaches and no further. A shadow ray
            // crosses this height half way to the lamp, so what it covers is every point of the ray
            // short of the lamp's own y and nothing beyond it.
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                .mMesh = scene.addMesh(makeLid(Fixture::sLidHeight, -Fixture::sDistance, Fixture::sLamp.y(), 2000.0f),
                    {}, {}, sQuadIndices) });

            scene.addLight(Light{
                .mPosition = Fixture::sLamp,
                .mIntensity = Fixture::sIntensity,
                .mReach = Fixture::sReach,
            });

            Shaders::VisibilityConstants camera = makeCamera(osg::Vec3f(0.0f, -Fixture::sDistance, 0.0f),
                osg::Vec3f(0.0f, 0.0f, 0.0f), 10.0f, size, size, 100000.0f);
            litThroughFog(camera, Fixture::sExtinction);
            camera.mFogUniform = sVolumeOverEvenAir;

            // **Nothing in the frame but the lit air**, which is what makes the figure below a
            // figure about the air. Black fog, a black sky and no ambient leave the wall unlit and
            // every ray that misses it black — where a lit wall would put its own bounce in the
            // pixel, and a single sample of a bounce with no denoiser over it moves further between
            // frames than anything this is measuring.
            camera.mFogColour = osg::Vec3f();
            camera.mSkyHorizon = osg::Vec3f();
            camera.mSkyZenith = osg::Vec3f();
            camera.mAmbientFromSky = 0.0f;

            std::vector<float> radiance;
            radianceFrameByFrame(scene, camera, size, frames, centre, radiance);

            double total = 0.0;
            double stepped = 0.0;
            for (std::size_t frame = frames - settled; frame < frames; ++frame)
            {
                total += double{ radiance[frame] };
                stepped += std::abs(double{ radiance[frame] } - double{ radiance[frame - 1] });
            }

            const double mean = total / double{ settled };
            const double step = stepped / double{ settled };

            // The lamp has to be lighting the air, or a frame that stands perfectly still is a
            // frame with nothing in it.
            ASSERT_GT(mean, 0.02) << "the lit air the flicker is measured against";

            EXPECT_LT(step / mean, 0.03) << "how far a settled pixel of lit air moves between frames";
        }

        /// The volume lights the air up to a surface, wherever inside a slice the surface stands.
        ///
        /// **Two ways a froxel grid gets the last slice wrong, and one measurement for both.** The
        /// slice a surface stands inside is sampled on both sides of it unless the sampling knows
        /// where the surface is — `fogdepth.comp` says what that drew — and what a pixel reads
        /// between two slices' edges is a shape the integrate pass has to have agreed to, which
        /// `FogSlice` says. Either error is a function of where inside its slice the surface
        /// stands, so the wall is put at four depths across three slices and the volume is held
        /// against the closed form at each: half and nine tenths of the way through the slice from
        /// 732 to 886 units, a tenth of the way through the next, and a quarter of the way through
        /// the one from 1055.
        ///
        /// **The lamp stands in front of the wall at every one of them**, so a draw that lands
        /// behind the wall finds the lamp hidden by it — which is the contamination a froxel
        /// sampled on both sides of its surface carries, at its strongest where the lamp is
        /// brightest. And not behind it: the closed form judges a whole ray by one shadow ray from
        /// the lamp's closest approach, and a wall standing between that point and the lamp is a
        /// question that one ray answers for none of the rest.
        ///
        /// **And the wall's own light is taken off both paths**, because a lamp in front of the
        /// wall lights it, identically on either. A frame with no air in it measures what the wall
        /// alone is worth, and what the air in front of it takes of that is the transmittance the
        /// closed form states.
        ///
        /// **Five per cent, where the estimator sampled on both sides of the wall lost between
        /// seven and eleven per cent of the lit air at the first three depths**, and the line the
        /// sampler draws through an inverse square is a few per cent from it over a slice at the
        /// lamp's closest approach.
        TEST_F(RtxVisibilityTest, theVolumeLightsTheAirUpToASurfaceWhereverInASliceItStands)
        {
            using Fixture = LampInTheAir;

            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreOf(size);
            constexpr std::size_t frames = 48;
            constexpr std::size_t settled = 16;

            // The lamp stands where the fixture puts it relative to the eye, whatever the wall's
            // distance, so the air it lights is the same air in every frame and only where the wall
            // cuts it off moves.
            const float ahead = Fixture::sDistance + Fixture::sLamp.y();

            const auto settle = [&](float distance, bool banked, bool fogged) {
                SceneDesc scene = makeWall();
                scene.addLight(Light{
                    .mPosition = osg::Vec3f(0.0f, ahead - distance, Fixture::sLamp.z()),
                    .mIntensity = Fixture::sIntensity,
                    .mReach = Fixture::sReach,
                });

                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -distance, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 10.0f, size, size, 100000.0f);
                litThroughFog(camera, fogged ? Fixture::sExtinction : 0.0f);
                camera.mFogUniform = banked ? sVolumeOverEvenAir : 1.0f;

                // Nothing in the frame but the lamp: black air, a black sky and no ambient, for the
                // reason the test above gives.
                camera.mFogColour = osg::Vec3f();
                camera.mSkyHorizon = osg::Vec3f();
                camera.mSkyZenith = osg::Vec3f();
                camera.mAmbientFromSky = 0.0f;

                std::vector<float> radiance;
                radianceFrameByFrame(scene, camera, size, frames, centre, radiance);

                double total = 0.0;
                for (std::size_t frame = frames - settled; frame < frames; ++frame)
                    total += double{ radiance[frame] };
                return total / double{ settled };
            };

            for (const float distance : { 809.0f, 871.0f, 903.0f, 1101.0f })
            {
                const double wall = settle(distance, false, false);
                const double through = std::exp(-double{ Fixture::sExtinction } * double{ distance });

                const double closed = settle(distance, false, true) - wall * through;
                const double volume = settle(distance, true, true) - wall * through;

                ASSERT_GT(closed, 0.01) << "the lit air the volume is measured against, at " << distance;
                EXPECT_NEAR(volume / closed, 1.0, 0.05)
                    << "the volume's lit air against the closed form's, at " << distance;
            }
        }

        /// The banked field holds as much air as an even one, which is what `FOG_COVERAGE` is for.
        ///
        /// **The noise redistributes the fog, it does not remove it.** The extinction the host
        /// derived is Morrowind's own view distance turned into a coefficient, and a band that
        /// leaves 40% of the volume clear would silently make the world twice as clear as the game
        /// says. Dividing the coverage by its own mean is what holds the average where it was — and
        /// this is what makes that constant a measurement rather than a note, since moving the band
        /// without re-measuring moves this ratio by the constant's own error.
        ///
        /// **It settles just under one, and two things put it there rather than a mistake.** A
        /// banked field's optical depth varies far more than an even one's, and `exp` is convex, so
        /// more light survives the same *average* density. And a far step still reads a coarser
        /// level of the field than a near one, which `FOG_FIELD_COARSEST` holds to a twentieth
        /// rather than removing — `everyLevelAMarchMayReadClearsTheShareTheDensityIsDividedBy` is
        /// what measures that half and says where the cap goes.
        ///
        /// Measured at 0.969 against a thickness of 0.09, and the convex half of the deficit scales
        /// with that thickness, so a thinner fog would sit closer to one.
        ///
        /// Nine viewpoints, because one is not a sample: the steps bunch near the camera, so a
        /// single frame weighs one small volume of the field heavily and lands anywhere within six
        /// per cent. Nine brings that to about two.
        TEST_F(RtxVisibilityTest, theBankedFieldHoldsAsMuchAirAsAnEvenOne)
        {
            constexpr std::uint32_t size = 64;
            constexpr std::size_t count = std::size_t{ size } * size;

            const auto air = [&](float uniform, float where) {
                Shaders::VisibilityConstants camera = makeCamera(osg::Vec3f(where, -50000.0f, 0.0f),
                    osg::Vec3f(where, -60000.0f, 0.0f), 90.0f, size, size, 100000.0f);
                camera.mFogUniform = uniform;

                std::vector<float> luminance;
                airThrough(camera, size, luminance);

                double total = 0.0;
                for (const float value : luminance)
                    total += double{ value };

                return total / static_cast<double>(count);
            };

            double ratio = 0.0;
            constexpr std::array places{ 0.0f, 12345.0f, -31000.0f, 77000.0f, 250000.0f, -140000.0f, 33000.0f,
                -420000.0f, 610000.0f };
            for (const float where : places)
                ratio += air(0.0f, where) / air(1.0f, where);

            ratio /= static_cast<double>(places.size());

            EXPECT_NEAR(ratio, 0.969, 0.05) << "banked air against even air, over nine viewpoints";
        }

        /// The wind carries the banks downwind, and a camera that walks with the wind sees the air
        /// stand still.
        ///
        /// **Exact rather than a threshold, because advection is a translation.** The field is read
        /// at `position - wind * time * FOG_GALE`, so an eye moved by exactly that much at the same
        /// moment reads the field the unmoved eye reads with no wind at all — churn and all, since
        /// the churn is a function of the moment and not of the wind. Nothing else in this frame
        /// knows where the eye is: the wall is behind it, the rays reach the sky, and the layer is a
        /// function of height alone.
        ///
        /// **And the sign is the half that matters.** Sampling from further *upwind* as the clock
        /// runs is what carries a bank past; adding would walk the whole field into the wind. A
        /// camera moved the wrong way sees a different field, which the last assertion checks.
        TEST_F(RtxVisibilityTest, theWindCarriesTheFieldAndAnEyeThatWalksWithItSeesItStandStill)
        {
            constexpr std::uint32_t size = 64;
            constexpr std::size_t count = std::size_t{ size } * size;
            constexpr float seconds = 2.0f;
            const osg::Vec2f wind(0.3f, 0.4f);

            const auto frame = [&](const osg::Vec2f& blowing, const osg::Vec3f& eye) {
                Shaders::VisibilityConstants camera
                    = makeCamera(eye, eye + osg::Vec3f(0.0f, -10000.0f, 0.0f), 90.0f, size, size, 100000.0f);
                camera.mFogUniform = 0.0f;
                camera.mFogWind = blowing;
                camera.mTime = seconds;

                std::vector<float> luminance;
                airThrough(camera, size, luminance);
                return luminance;
            };

            const auto apart = [&](const std::vector<float>& a, const std::vector<float>& b) {
                double total = 0.0;
                for (std::size_t i = 0; i < count; ++i)
                    total += std::abs(double{ a[i] } - double{ b[i] });
                return total / static_cast<double>(count);
            };

            const osg::Vec3f eye(0.0f, -50000.0f, 0.0f);
            const osg::Vec3f carried(
                wind.x() * seconds * Shaders::FOG_GALE, wind.y() * seconds * Shaders::FOG_GALE, 0.0f);

            const std::vector<float> still = frame(osg::Vec2f(), eye);
            const std::vector<float> downwind = frame(wind, eye + carried);
            const std::vector<float> upwind = frame(wind, eye - carried);

            // The two read one field at one moment, and differ by the rounding of an eye moved
            // fourteen hundred units against a ray that runs thirty thousand.
            EXPECT_LT(apart(still, downwind), 1e-3) << "an eye walking with the wind sees the air stand still";

            // The wrong sign is two different fields, and the gap between them is the banks
            // themselves. Measured at 0.03 of the frame's own mean of about 0.5 by the march, and at
            // 0.0087 by the volume, whose tent over a frame eight columns wide and whose line from
            // one slice to the next both smooth a single frame's draws — which is what they are for
            // — against a gap of exactly nought for the right sign.
            EXPECT_GT(apart(still, upwind), 0.005) << "an eye walking against the wind sees another air";
        }

        /// The fog scatters the sun forward far harder than back, which is what a Mie phase is for.
        ///
        /// **A single Henyey-Greenstein lobe cannot do this shape.** Real droplets throw a peak
        /// within a degree of the light that is orders of magnitude above anything one `g` reaches,
        /// and still send a sixth of isotropic *backwards* — the blaze around a low sun, and fog not
        /// going black when you turn away from it. Before this the fog was lit by the sky and the
        /// lamps and not at all by the sun, so facing it rendered identically to facing away.
        ///
        /// **A ratio, because it is the only thing the fixture computes exactly.** Two frames differ
        /// in nothing but which side of the camera the sun is on, at the same elevation — so the
        /// column of fog it crosses, the extinction, the transmittance and the march all cancel, and
        /// what is left is `p(26.6 degrees) / p(153.4)`. An isotropic fog would give exactly one.
        TEST_F(RtxVisibilityTest, theFogScattersTheSunForwardFarHarderThanBack)
        {
            // **Wider than the 33 every other test here uses, because the volume answers per
            // column.** A column is eight pixels across and holds the air along *its own* ray, so
            // at 33 pixels the column the centre pixel reads points six degrees off that pixel's,
            // climbs out of the layer, and carries air a fifth thinner than the level ray the
            // closed form below is written for. The bias falls with the frame — 0.63 at 33 pixels,
            // 0.53 at 129, 0.515 at 257 and 0.509 at 513, against the 0.4999 it is going to — and
            // at 1920 by 1080 a column is a quarter of a degree wide. The ratio does not care,
            // since the transport it divides out is the column's either way.
            constexpr std::uint32_t size = 257;
            constexpr std::size_t centre = centreValueOf(size);

            // Bright enough to read against eight bits after the fog's own column has taken 83% of
            // it, and the forward case still has to stay inside one.
            constexpr float irradiance = 12.7f;

            // Level, so the centre ray holds one height and the medium along it is uniform — which
            // is what lets the two frames cancel to the phase function alone.
            const auto lookPast = [&](float towardsY) {
                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, 0.0f, 0.0f), osg::Vec3f(0.0f, 1000.0f, 0.0f), 60.0f, size, size, 100000.0f);

                // The sun a quarter of the way up, ahead of the camera or behind it. Both carry the
                // same climb, so `fogSunDepth` is the same for each and cancels.
                osg::Vec3f towards(0.0f, towardsY, 0.5f);
                towards.normalize();
                camera.mSunPosition = towards;
                camera.mSunIrradiance = osg::Vec3f(irradiance, irradiance, irradiance);
                camera.mFogExtinction = 3.0e-4f;
                camera.mFogUniform = 1.0f;

                // Nothing to hit and nothing else to scatter: every ray runs out to `FOG_REACH`
                // through even air whose own colour is black, so the pixel is the sun and no more.
                //
                // **Out of reach of the shadow rays and not merely out of the frame.** The march
                // sends one at the sun from every stretch of the ray, and a wall standing at y = 0
                // is squarely in the path of the ones the *backward* case sends — so it shadowed the
                // near steps, at a march offset that decides which, and the ratio below moved by a
                // sixth with the dither. A sheet below the world is past `mFar` in every direction
                // any ray here travels.
                SceneDesc scene;
                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                    .mMesh = scene.addMesh(sheetAt(4000.0f, -200000.0f), {}, {}, sQuadIndices) });

                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels);

                return decodeSrgb(pixels[centre]);
            };

            const float ahead = lookPast(1.0f);
            const float behind = lookPast(-1.0f);

            // Hand-computed from Jendersie and d'Eon's fit at a droplet diameter of eight microns:
            // the blend is 47.4% Draine over a Henyey-Greenstein peak of g = 0.98447, and at
            // `cos = +-0.8944` that comes to 0.225158 forward against 0.011708 back.
            constexpr float forward = 0.225158f;
            constexpr float backward = 0.011708f;

            EXPECT_NEAR(ahead / behind, forward / backward, 1.0f) << "forward against backward scattering";

            // **And the absolute value, because a ratio cannot see a factor of `4 pi`.** That is the
            // mistake this shape of function invites: normalise the phase so isotropic reads one —
            // the convention a lamp is written in — and both frames grow together while their ratio
            // stays put. What the eye gets is the sun's irradiance times the phase *per steradian*,
            // less the fog's own column on the way down, over a ray that runs to `FOG_REACH`:
            //
            //   12.7 * 0.225158 * exp(-2600 * 3e-4 / 0.44721) * (1 - exp(-3e-4 * 30000)) = 0.4998
            //
            // which the sRGB curve puts at 188 of 255. Four pi times that is white.
            //
            // **The tolerance is what a column eight pixels wide has left over**, which the frame
            // size above is about: 0.515 measured against 0.4999 here, and falling as the frame
            // grows rather than sitting where it is.
            const float climb = 0.5f / std::sqrt(1.25f);
            const float column = std::exp(-Shaders::FOG_HEIGHT * 3.0e-4f / climb);
            const float crossed = 1.0f - std::exp(-3.0e-4f * Shaders::FOG_REACH);

            EXPECT_NEAR(ahead, irradiance * forward * column * crossed, 0.02f)
                << "the sun's own irradiance through the phase function, per steradian";

            // And the backward half is not nothing, which is the other half of why a single lobe
            // will not do: fog behind you still glows.
            EXPECT_GT(behind, 0.01f) << "a sixth of isotropic still comes back";
        }

        /// A lid over the march takes the sun out of the air beneath it, and takes all of it.
        ///
        /// **Exactly what sunless air scatters, not merely less than open air.** A shaft that leaked
        /// a tenth of the sun would still be darker than the open sky beside it, and an assertion
        /// that only asked for darker would pass while it leaked. The lid spans the whole march and
        /// the camera's own ray never reaches it — it runs level while the lid is five hundred units
        /// overhead — so what changes between the two frames is the shadow ray and nothing else.
        TEST_F(RtxVisibilityTest, aLidOverTheMarchTakesTheSunOutOfTheAirBeneathIt)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);
            constexpr float irradiance = 4.0f;

            const auto look = [&](bool lidded, bool lit) {
                // The same sheet either way, over the march or under it, so the two frames differ
                // in what the shadow ray finds and in nothing else — not in what is in the scene,
                // nor in how large it is.
                SceneDesc scene;
                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                    .mMesh = scene.addMesh(sheetAt(40000.0f, lidded ? 500.0f : -500.0f), {}, {}, sQuadIndices) });

                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, 0.0f, 0.0f), osg::Vec3f(0.0f, 1000.0f, 0.0f), 60.0f, size, size, 100000.0f);

                // Ahead of the camera and well up, so the phase function has something to scatter
                // forward and every shadow ray still climbs into the lid.
                osg::Vec3f travelling(0.0f, -0.6f, -0.8f);
                travelling.normalize();
                camera.mSunPosition = -travelling;
                camera.mSunIrradiance = lit ? osg::Vec3f(irradiance, irradiance, irradiance) : osg::Vec3f();

                // Even air with a colour of its own, so the frame is never empty and the two sunless
                // cases have something to agree about.
                camera.mFogColour = osg::Vec3f(0.02f, 0.02f, 0.02f);
                camera.mFogExtinction = 2.0e-4f;
                camera.mFogUniform = 1.0f;

                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels);
                return int{ pixels[centre] };
            };

            const int open = look(false, true);
            const int shaded = look(true, true);
            const int sunless = look(true, false);

            EXPECT_EQ(shaded, sunless) << "the lid takes all of the sun, not most of it";
            EXPECT_GT(open, shaded + 20) << "and there was a sun to take";
        }

        /// A sprite fades through the layer's own integral, not through one sample of it.
        ///
        /// **A descending ray is what tells the two apart.** Along a level ray the air holds one
        /// density and a sample anywhere in it is the whole answer, so the midpoint the sprite walk
        /// took was exact. A ray that drops three scale heights crosses a layer that thickens under
        /// it, and the mean of an exponential over that stretch is not the exponential at its middle
        /// — so a puff of smoke and the wall behind it, one distance from the eye, faded at two
        /// rates.
        ///
        /// **An adding sprite, because what it puts into the pixel is the transmittance times a
        /// constant.** `spritesAlong` writes `mAdded` as `(1 - addedThrough) * FLAME_INTENSITY`, and
        /// for one sprite that is its own `glow` — the texel, the paint and the chord, all of them
        /// the same in both legs, times `reaching`. A covering sprite is lit by the froxel it stands
        /// in, and the froxel moves when the fog does.
        ///
        /// The camera stands three scale heights up and looks down at forty-five degrees to a sprite
        /// on the base, so the ray runs `7800 * sqrt(2)` = 11030.87 units from `z = 7800` to
        /// `z = 0`. Over that stretch the layer's mean falloff is
        ///
        ///   (exp(-3) - exp(0)) / (0 - 3) = 0.3167376
        ///
        /// so the optical depth is `3e-4 * 11030.87 * 0.3167376` = 1.048177 and what is left of the
        /// sprite is `exp(-1.048177)` = 0.35058. One sample at the middle reads `exp(-1.5)` =
        /// 0.2231302 instead, for a depth of 0.738438 and a transmittance of 0.47784 — a third more
        /// of the sprite than the air leaves.
        TEST_F(RtxVisibilityTest, aSpriteFadesThroughTheWholeLayerAndNotOneSampleOfIt)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);
            const float height = 3.0f * Shaders::FOG_HEIGHT;

            constexpr std::array<std::uint8_t, 4> half{ 255, 255, 255, 128 };
            const std::array<TextureData, 1> flame{ describeTexel(half) };

            const auto glow = [&](float extinction) {
                SceneDesc scene;
                const Index cut = scene.addTexture(VFS::Path::NormalizedView("sprite.dds"));

                // Wide enough to fill the middle of the frame from three scale heights away, which
                // is what the descent costs in distance.
                const std::array<Sprite, 1> sprites{ Sprite{
                    .mPosition = osg::Vec3f(0.0f, 0.0f, 0.0f), .mRadius = 2000.0f, .mAlpha = 1.0f } };
                scene.addEmitter(sprites, cut, true);

                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -height, height), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);

                // Nothing but the sprite in the pixel: black air adds no colour of its own, and an
                // unlit sky leaves the ray past the sprite carrying nothing.
                camera.mSkyHorizon = osg::Vec3f();
                camera.mSkyZenith = osg::Vec3f();
                camera.mAmbient = osg::Vec3f();
                camera.mSunIrradiance = osg::Vec3f();
                camera.mFogColour = osg::Vec3f();
                camera.mFogExtinction = extinction;

                // Even air, which is the case the closed form is exact for: with no coverage band
                // along the ray, the height falloff is the whole of what varies.
                camera.mFogUniform = 1.0f;

                std::vector<std::uint8_t> pixels;
                countHits(scene, flame, camera, size, pixels);

                // The radiance and not the byte: what is asserted is a ratio, and eight bits of a
                // display curve is a coarse place to take one.
                return mRadiance[centre];
            };

            const float clear = glow(0.0f);
            ASSERT_GT(clear, 0.02f) << "the sprite has to be in the frame before its fading means anything";

            const float hazed = glow(3.0e-4f);

            EXPECT_NEAR(hazed / clear, 0.35058f, 0.02f) << "the layer's integral over the whole descent";
        }

        /// The air behind a pane is the air that is there.
        ///
        /// **A column of the volume ends where the eye's own ray ends, and the eye sees through
        /// glass.** `fogdepth.comp` stopped each column at the first surface its ray met, so every
        /// slice past a pane was left as it stood and the room behind a window carried no air at
        /// all: a wall four thousand units off behind a pane at one thousand kept 0.66 of its light
        /// where 0.25 is what the air leaves it, and taking the pane out of the scene put it back.
        ///
        /// **A ratio against the same scene in clear air**, so the pane's own half and the wall's
        /// own radiance divide out and what is left is the transmittance. The wall glows, for the
        /// reason the test below gives: a figure no shadow, no ambient and no bounce can move.
        TEST_F(RtxVisibilityTest, theAirBehindAPaneIsTheAirThatIsThere)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);
            constexpr float wallAway = 4000.0f;
            constexpr float extinction = 3.5e-4f;

            const auto look = [&](bool paned, float thickness) {
                SceneDesc scene;

                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                    .mMesh = scene.addMesh(wallAt(0.0f), {}, {}, sQuadIndices),
                    .mMaterial = scene.addMaterial(Material{
                        .mDiffuseColour = osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f),
                        .mEmissiveColour = osg::Vec3f(1.0f, 1.0f, 1.0f),
                    }) });

                // A quarter of the way along the path, and wide enough to fill the middle of the
                // frame from there.
                if (paned)
                    addPane(scene, uprightQuadAt(2400.0f, 1000.0f - wallAway), osg::Vec4f(0.0f, 0.0f, 0.0f, 0.5f));

                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -wallAway, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
                litThroughFog(camera, thickness);

                // Nothing in the pixel but the wall's own glow through the air: a black pane
                // reflects nothing, and air with no colour of its own scatters none in.
                camera.mSkyHorizon = osg::Vec3f();
                camera.mSkyZenith = osg::Vec3f();
                camera.mAmbient = osg::Vec3f();
                camera.mSunIrradiance = osg::Vec3f();
                camera.mFogColour = osg::Vec3f();

                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels);

                return mRadiance[centre];
            };

            // The volume's own quadrature stands this a little over the closed form, which is what
            // the tolerance is: `exp(-3.5e-4 * 4000)` is 0.2466 and it reads 0.268.
            const float open = look(false, extinction) / look(false, 0.0f);
            EXPECT_NEAR(open, std::exp(-extinction * wallAway), 0.03f) << "the wall through four thousand units";

            // **The same air, and the pane changed none of it.** Exactly, because the two frames
            // differ in what stands at a thousand units and in nothing about the air.
            EXPECT_NEAR(look(true, extinction) / look(true, 0.0f), open, 1.0e-3f) << "and the same behind a pane";
        }

        /// A pane is hazed over its own distance and not over the path behind it.
        ///
        /// **Two stretches of one path, split at the glass.** The composite ran before the water
        /// and the air rather than after them, so the pane was multiplied by the transmittance
        /// measured to the surface *behind* it — and a lit window a few units out arrived as dim as
        /// the wall four thousand units further on. Moving the glass along the path changed nothing
        /// at all.
        ///
        /// **A surface that glows on its own, because a lit one brings its own questions.** What is
        /// asserted is a transmittance, so the radiance under it has to be a figure no shadow, no
        /// ambient term and no bounce can move. The glow joins the light rather than the albedo, so
        /// the wall behind carries an albedo of nought and stays dark whatever the scene puts on it,
        /// and the pane's own figure divides back out of the ratio against the same scene in clear
        /// air.
        ///
        /// The eye stands 4000 units from the wall, and the pane is held at 1000 units and again at
        /// 3000, so over air of even density and an extinction of 3.5e-4 the two answers are
        /// `exp(-0.35)` = 0.70469 and `exp(-1.05)` = 0.34994. Composited the old way both read the
        /// wall's own column instead, and read the same number as each other.
        ///
        /// **And the same two figures with a second pane in front of it**, which is the same fault
        /// one layer deeper: the stack was charged the medium in front of its *nearest* layer, so a
        /// glowing pane behind a plain one at five hundred units read as though it stood at five
        /// hundred, and moving it along the path changed nothing at all. The plain pane halves both
        /// frames and divides out of the ratio.
        ///
        /// **The tolerances are the volume's own quadrature and nothing else.** Its slices are
        /// quadratic in depth, so a longer stretch is read across coarser ones: the near answer
        /// stands 0.003 over the closed form and the far one 0.014, and each tolerance is the next
        /// round figure above its own.
        TEST_F(RtxVisibilityTest, aPaneIsHazedOverItsOwnDistanceAndNotOverThePathBehindIt)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);
            constexpr float wallAway = 4000.0f;
            constexpr float extinction = 3.5e-4f;

            const std::array<osg::Vec3f, 4> wall = wallAt(0.0f);

            const auto glow = [&](float paneAway, float thickness, bool behindAnother = false) {
                SceneDesc scene;

                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                    .mMesh = scene.addMesh(wall, {}, {}, sQuadIndices),
                    .mMaterial = scene.addMaterial(Material{
                        .mDiffuseColour = osg::Vec4f(0.0f, 0.0f, 0.0f, 1.0f),
                    }) });

                // Wide enough to fill the middle of the frame from the far end of the path, where
                // a sixty-degree frame covers 1732 units either side of the axis.
                const std::array<osg::Vec3f, 4> pane = uprightQuadAt(2400.0f, paneAway - wallAway);
                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                    .mMesh = scene.addMesh(pane, {}, {}, sQuadIndices),
                    .mMaterial = scene.addMaterial(Material{
                        .mDiffuseColour = osg::Vec4f(1.0f, 1.0f, 1.0f, 0.5f),
                        .mEmissiveColour = osg::Vec3f(1.0f, 1.0f, 1.0f),
                        .mAlphaMode = AlphaMode::Blend,
                    }) });

                // Half way to the glowing one at its nearest, and black, so what it puts into the
                // pixel is nothing and what it does to the pane behind it is a half.
                if (behindAnother)
                    addPane(scene, uprightQuadAt(2400.0f, 500.0f - wallAway), osg::Vec4f(0.0f, 0.0f, 0.0f, 0.5f));

                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -wallAway, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
                litThroughFog(camera, thickness);

                // Nothing in the pixel but the glass: black air scatters no colour of its own, and
                // an unlit sky leaves both surfaces with nothing to reflect.
                camera.mSkyHorizon = osg::Vec3f();
                camera.mSkyZenith = osg::Vec3f();
                camera.mAmbient = osg::Vec3f();
                camera.mSunIrradiance = osg::Vec3f();
                camera.mFogColour = osg::Vec3f();

                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels);

                return mRadiance[centre];
            };

            const float nearClear = glow(1000.0f, 0.0f);
            ASSERT_GT(nearClear, 0.02f) << "the pane has to be in the frame before its fading means anything";
            EXPECT_NEAR(glow(1000.0f, extinction) / nearClear, 0.70469f, 0.005f) << "a thousand units of air";

            const float farClear = glow(3000.0f, 0.0f);
            ASSERT_NEAR(farClear, nearClear, 1.0e-4f) << "the same glass, and only the air in front of it moved";
            EXPECT_NEAR(glow(3000.0f, extinction) / farClear, 0.34994f, 0.02f) << "three thousand units of it";

            // The same two, each behind a pane of its own at five hundred units.
            EXPECT_NEAR(glow(1000.0f, extinction, true) / glow(1000.0f, 0.0f, true), 0.70469f, 0.005f)
                << "a thousand units of air, behind a second pane";
            EXPECT_NEAR(glow(3000.0f, extinction, true) / glow(3000.0f, 0.0f, true), 0.34994f, 0.02f)
                << "three thousand units of it, behind a second pane";
        }

        /// A moon too faint for a shadow ray still lights the air.
        ///
        /// **`FOG_SHAFT_FLOOR` is a threshold on the ray and not on the light.** It asks whether a
        /// shaft cut out of what the pair delivers would be visible at all, and ninety degrees off a
        /// moon it is not. The scatter pass read the same flag to decide whether the moons lit the
        /// air, so both of them left the frame together the moment their share of the sky's term
        /// crossed the threshold, rather than losing only their shadow.
        ///
        /// **And they left it one column at a time.** `fogSourcesAlong` takes the phase from the
        /// column's own direction, so neighbouring columns sit either side of the threshold and the
        /// tent the integrate pass reads averages a lit column with a black one. What that draws is
        /// a seam across the air rather than a step in time.
        ///
        /// **Measured in a channel the air's own colour barely holds.** The threshold is
        /// `FOG_SHAFT_FLOOR` of the *brightest* channel of the fog colour, so an air bright in red
        /// and green and near black in blue puts the crossing far above what the air itself puts
        /// into blue. An even grey would put the whole effect under the rounding of an eight-bit
        /// channel.
        ///
        /// **What is asserted is the light per unit of irradiance**, which is bounded above and
        /// below rather than fixed: a leg under the threshold casts no ray and arrives unshadowed,
        /// so it delivers *more* per unit than one that pays for its own shadow, and the fog's own
        /// beam is what stands between the two. Nothing may deliver less than the shadowed leg, and
        /// nothing may deliver more than twice it. The step this is about reads nought.
        TEST_F(RtxVisibilityTest, aMoonTooFaintForItsOwnShadowStillLightsTheAir)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);

            // Bright in red and green so the threshold is high, and near black in blue so the moon
            // has a channel of its own to be read in.
            const osg::Vec3f haze(0.4f, 0.4f, 0.001f);

            // A decade apart, which is what puts the threshold inside the sweep: measured here, the
            // brightest two legs are worth their ray and the faintest is not.
            constexpr std::array<float, 4> irradiances{ 0.001f, 0.01f, 0.1f, 1.0f };

            const auto lit = [&](float irradiance) {
                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, 0.0f, 0.0f), osg::Vec3f(0.0f, 1000.0f, 0.0f), 60.0f, size, size, 100000.0f);

                camera.mFogColour = haze;
                camera.mFogExtinction = 3.0e-4f;
                camera.mFogUniform = 1.0f;

                // **No disc, because what is measured is the air and not the moon.** A disc drawn
                // where the ray runs would put the moon's own radiance into the same pixel, and the
                // irradiance alone still carries `HAS_MOONS` — so the basis, the colour and the face
                // a disc would be cut from are all left at nothing. `mLimb` is not one of those: it
                // is how wide the shadow ray's cone opens.
                Shaders::MoonDisc moon{};
                osg::Vec3f towards(0.0f, 1.0f, 0.5f);
                towards.normalize();
                moon.mDirection = towards;
                moon.mIrradiance = osg::Vec3f(0.0f, 0.0f, irradiance);
                moon.mLimb = std::sin(moonAngularRadius(Moon::Masser));
                moon.mAlpha = 0.0f;
                moon.mFace = Shaders::NO_TEXTURE;
                camera.mMoons[0] = moon;

                // The sheet is past `mFar` in every direction a ray here travels, for the reason
                // `theFogScattersTheSunForwardFarHarderThanBack` gives: a wall in the path of a
                // shadow ray would shadow what this measures.
                SceneDesc scene;
                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                    .mMesh = scene.addMesh(sheetAt(4000.0f, -200000.0f), {}, {}, sQuadIndices) });

                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels);

                // Blue is the third of the pixel's four values, and the channel the moon has to
                // itself.
                return decodeSrgb(pixels[centre + 2]);
            };

            const float dark = lit(0.0f);

            // The brightest leg is far above the threshold and pays for its own shadow, so it is the
            // floor every fainter leg is measured against.
            const float shadowed = (lit(irradiances.back()) - dark) / irradiances.back();
            EXPECT_GT(shadowed, 0.0f) << "a moon well above the threshold lit the air";

            for (const float irradiance : irradiances)
            {
                const float delivered = (lit(irradiance) - dark) / irradiance;

                EXPECT_GE(delivered, shadowed) << "the air went dark at an irradiance of " << irradiance;
                EXPECT_LE(delivered, 2.0f * shadowed)
                    << "more than the moon's own shadow was worth, at an irradiance of " << irradiance;
            }
        }
    }
}
