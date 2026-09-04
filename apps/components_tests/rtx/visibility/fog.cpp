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
        enum class AirLight
        {
            Sun,
            Masser,
            Secunda,
        };

        void lightAir(
            Shaders::VisibilityConstants& camera, AirLight source, const osg::Vec3f& direction, float irradiance)
        {
            const osg::Vec3f energy(irradiance, irradiance, irradiance);
            if (source == AirLight::Sun)
            {
                camera.mSunPosition = direction;
                camera.mSunIrradiance = energy;
            }
            else
            {
                Shaders::MoonDisc& moon = camera.mMoons[source == AirLight::Masser ? 0 : 1];
                moon.mDirection = direction;
                moon.mIrradiance = energy;
            }
        }

        /// What colour the air is. Named for the reason `sFoggySky` is: each expectation computes
        /// with it as well as handing it to the shader. Deliberately not grey, so a fog scattering
        /// the wrong colour cannot pass by matching a total.
        const osg::Vec3f sHaze(0.1f, 0.2f, 0.4f);

        // Exercise the noise field within 0.2% of uniform density; analytic tests use exactly one.
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

            constexpr std::array<std::uint8_t, 4> black{ 0, 0, 0, 255 };
            const std::array<TextureData, 1> texture{ describeTexel(black) };
            Shaders::VisibilityConstants camera
                = makeCamera(osg::Vec3f(0.0f, -distance, 0.0f), osg::Vec3f(), 60.0f, size, size, 100000.0f);
            litThroughFog(camera, extinction);

            // A black half-opaque puff dims the wall and the haze behind it, leaving the near
            // haze intact. The exact boundaries here are 30000 * (3.5/64)^2 and (9.5/64)^2.
            constexpr float firstSplit = 89.7216796875f;
            constexpr float secondSplit = 661.0107421875f;
            for (const float seen : { 20.0f, firstSplit - 1.0f, firstSplit, firstSplit + 1.0f, secondSplit - 1.0f,
                     secondSplit, secondSplit + 1.0f, 1900.0f })
            {
                SCOPED_TRACE(seen);
                SceneDesc scene = makeWall();
                const Index cut = scene.addTexture(VFS::Path::NormalizedView("sprite.dds"));
                const std::array<Sprite, 1> sprites{ Sprite{
                    .mPosition = osg::Vec3f(0.0f, -distance + seen, 0.0f), .mRadius = 10.0f, .mAlpha = 0.5f } };
                scene.addEmitter(sprites, cut, false);
                std::vector<std::uint8_t> pixels;
                countHits(scene, texture, camera, size, pixels);

                const float front = std::exp(-extinction * seen);
                for (std::size_t channel = 0; channel < 3; ++channel)
                {
                    const float expected = 0.5f * wall * transmittance
                        + sHaze[channel] * ((1.0f - front) + 0.5f * (front - transmittance));
                    // Float accumulation through the volume; compare linear radiance before sRGB quantization.
                    EXPECT_NEAR(mRadiance[centre + channel], expected, 2.0e-5f) << "channel " << channel;
                }
            }
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
            const auto look = [&](float level, float thickness, float eye = 0.0f, float fov = 60.0f) {
                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -distance, eye), osg::Vec3f(0.0f, 0.0f, eye), fov, size, size, 100000.0f);
                litThroughFog(camera, thickness, level);

                const SceneDesc scene = makeWall();
                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels);
                return std::array<int, 3>{ pixels[centre], pixels[centre + 1], pixels[centre + 2] };
            };

            const std::array<int, 3> dry = look(-std::numeric_limits<float>::infinity(), extinction);
            for (const float fov : { 20.0f, 60.0f, 120.0f })
                for (const float level : { 0.0f, 100.0f })
                {
                    const std::array<int, 3> onSurface = look(level, extinction, level, fov);
                    for (std::size_t channel = 0; channel < 3; ++channel)
                        EXPECT_EQ(dry[channel], onSurface[channel])
                            << "channel " << channel << ", level " << level << ", fov " << fov;
                }

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

        // A submerged eye is entirely in the water medium, including pixels beside the waterline.
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

        TEST_F(RtxVisibilityTest, theVolumeLightsTheAirUpToASurfaceWhereverInASliceItStands)
        {
            using Fixture = LampInTheAir;
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);
            const double lampAhead = double{ Fixture::sDistance } + double{ Fixture::sLamp.y() };
            const double height = Fixture::sLamp.z();
            const double reach = Fixture::sReach;
            const double sigma = Fixture::sExtinction;
            const double intensity = Fixture::sIntensity.x();

            // Independent quadrature of sigma * Tview * I * falloff / (4*pi). The black wall
            // removes surface lighting, so there is no second render of the same fog code serving
            // as its own reference. Halving the integration step verifies the reference precision.
            const auto expected = [&](double distance, unsigned steps) {
                const double step = distance / steps;
                double sum = 0.0;
                for (unsigned i = 0; i <= steps; ++i)
                {
                    const double along = step * i;
                    const double span = std::hypot(height, along - lampAhead);
                    if (span >= reach)
                        continue;

                    const double window = 1.0 - std::pow(span / reach, 4.0);
                    const double value = sigma * std::exp(-sigma * along) * intensity * window * window
                        / ((span * span + 1.0) * 4.0 * std::acos(-1.0));
                    const double weight = i == 0 || i == steps ? 1.0 : (i % 2 == 0 ? 2.0 : 4.0);
                    sum += weight * value;
                }
                return step * sum / 3.0;
            };

            Material black;
            black.mDiffuseColour = osg::Vec4f(0.0f, 0.0f, 0.0f, 1.0f);
            std::vector<float> radiance;
            for (const float distance : { 809.0f, 871.0f, 903.0f, 1101.0f })
            {
                SceneDesc scene;
                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                    .mMesh = scene.addMesh(sWallQuad, {}, {}, sQuadIndices),
                    .mMaterial = scene.addMaterial(black) });
                scene.addLight(Light{ .mPosition = osg::Vec3f(0.0f, float(lampAhead) - distance, Fixture::sLamp.z()),
                    .mIntensity = Fixture::sIntensity,
                    .mReach = Fixture::sReach });
                Shaders::VisibilityConstants camera
                    = makeCamera(osg::Vec3f(0.0f, -distance, 0.0f), osg::Vec3f(), 10.0f, size, size, 100000.0f);
                camera.mAmbientFromSky = 0.0f;

                renderRadiance(scene, camera, size, radiance);
                EXPECT_EQ(radiance[centre], 0.0f) << "the black wall contributes no light";

                const double reference = expected(distance, 4096);
                EXPECT_NEAR(reference, expected(distance, 2048), 1.0e-8);
                for (const float uniform : { 1.0f, sVolumeOverEvenAir })
                {
                    camera.mFogExtinction = Fixture::sExtinction;
                    camera.mFogUniform = uniform;
                    renderRadiance(scene, camera, size, radiance);

                    // The ten-degree camera samples lamp irradiance from an eight-pixel grid;
                    // five percent covers that spatial approximation at the lamp's closest approach.
                    EXPECT_NEAR(double{ radiance[centre] }, reference, 0.05 * reference)
                        << "distance " << distance << ", uniform " << uniform;
                }
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

        TEST_F(RtxVisibilityTest, theFogScattersDirectionalLightByItsPhaseAndBothOpticalPaths)
        {
            constexpr float irradiance = 12.7f;
            constexpr float climb = 0.4472135955f;

            // Jendersie and d'Eon's eight-micron fit at cosine +/-0.89442719, per steradian.
            constexpr float forward = 0.22522234169f;
            constexpr float backward = 0.01170777396f;

            SceneDesc scene;
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                .mMesh = scene.addMesh(sheetAt(4000.0f, -200000.0f), {}, {}, sQuadIndices) });

            std::vector<float> radiance;
            for (const std::uint32_t size : { 1u, 33u })
                for (const float fov : { 20.0f, 120.0f })
                    for (const float extinction : { 1.5e-4f, 3.0e-4f })
                        for (const AirLight source : { AirLight::Sun, AirLight::Masser, AirLight::Secunda })
                        {
                            SCOPED_TRACE(static_cast<int>(source));
                            std::array<float, 2> scattered{};
                            for (const bool ahead : { true, false })
                            {
                                Shaders::VisibilityConstants camera = makeCamera(
                                    osg::Vec3f(), osg::Vec3f(0.0f, 1000.0f, 0.0f), fov, size, size, 100000.0f);
                                osg::Vec3f towards(0.0f, ahead ? 1.0f : -1.0f, 0.5f);
                                towards.normalize();
                                lightAir(camera, source, towards, irradiance);
                                camera.mFogExtinction = extinction;
                                camera.mFogUniform = 1.0f;
                                renderRadiance(scene, camera, size, radiance);

                                // A level ray holds one density: light transmittance exp(-sigma*H/climb)
                                // times the view's absorbed fraction. At sigma=3e-4 the forward result
                                // is 0.49991. The tolerance covers evaluation of the phase fit and
                                // single-precision arithmetic over 65 integration intervals.
                                const float column = std::exp(-Shaders::FOG_HEIGHT * extinction / climb);
                                const float crossed = -std::expm1(-extinction * Shaders::FOG_REACH);
                                const float expected = irradiance * (ahead ? forward : backward) * column * crossed;
                                const float actual = radiance[centreValueOf(size)];
                                EXPECT_NEAR(actual, expected, 2.0e-5f)
                                    << "size " << size << ", fov " << fov << ", extinction " << extinction << ", ahead "
                                    << ahead;
                                scattered[ahead ? 0 : 1] = actual;
                            }
                            EXPECT_GT(scattered[0], scattered[1]);
                            EXPECT_NEAR(scattered[0] / scattered[1], forward / backward, 0.001f);
                        }
        }

        /// A lid over the march takes the sun out of the air beneath it, and takes all of it.
        ///
        /// **Exactly what sunless air scatters, not merely less than open air.** A shaft that leaked
        /// a tenth of the sun would still be darker than the open sky beside it, and an assertion
        /// that only asked for darker would pass while it leaked. The lid spans the whole march and
        /// the camera's own ray never reaches it — it runs level while the lid is five hundred units
        /// overhead — so what changes between the two frames is the shadow ray and nothing else.
        TEST_F(RtxVisibilityTest, aLidTakesDirectionalLightOutOfTheAirBeneathIt)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);
            constexpr float irradiance = 4.0f;

            const auto look = [&](AirLight source, bool lidded, bool lit) {
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
                lightAir(camera, source, -travelling, lit ? irradiance : 0.0f);

                // Even air with a colour of its own, so the frame is never empty and the two sunless
                // cases have something to agree about.
                camera.mFogColour = osg::Vec3f(0.02f, 0.02f, 0.02f);
                camera.mFogExtinction = 2.0e-4f;
                camera.mFogUniform = 1.0f;

                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels);
                return int{ pixels[centre] };
            };

            for (const AirLight source : { AirLight::Sun, AirLight::Masser, AirLight::Secunda })
            {
                SCOPED_TRACE(static_cast<int>(source));
                const int open = look(source, false, true);
                const int shaded = look(source, true, true);
                const int unlit = look(source, true, false);

                EXPECT_EQ(shaded, unlit) << "the lid takes all of the directional light";
                EXPECT_GT(open, shaded + 20) << "the uncovered light scatters into the ray";
            }
        }
    }
}
