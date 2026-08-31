#include "fixture.hpp"

namespace Rtx::Testing
{
    namespace
    {
        /// What colour the air is. Named for the reason `sFoggySky` is: each expectation computes
        /// with it as well as handing it to the shader. Deliberately not grey, so a fog scattering
        /// the wrong colour cannot pass by matching a total.
        const osg::Vec3f sHaze(0.1f, 0.2f, 0.4f);

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
            // along the ray, and then the march stops telescoping and its answer stops being one
            // anyone can write down. The banks have their own test.
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
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;
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
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;
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

            // A dry cell is handed minus infinity, and falls back to sea level — which is where a
            // water level of zero puts the layer anyway, so the two have to agree exactly.
            const std::array<int, 3> dry = look(-std::numeric_limits<float>::infinity(), extinction);
            const std::array<int, 3> atSeaLevel = look(0.0f, extinction);
            for (std::size_t channel = 0; channel < 3; ++channel)
                EXPECT_EQ(dry[channel], atSeaLevel[channel]) << "channel " << channel << ", the dry-cell fallback";

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
            // half of the integral and the one a sign error would have shown up in.
            const std::array<int, 3> down
                = lookSloping(osg::Vec3f(0.0f, -distance, climb), osg::Vec3f(0.0f, 0.0f, 0.0f));
            for (std::size_t channel = 0; channel < 3; ++channel)
                EXPECT_EQ(down[channel], up[channel]) << "channel " << channel << ", the same heights descending";
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
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;
            constexpr float distance = 200.0f;
            constexpr float reach = 30000.0f;

            // Half the ray's worth of fog, so the wall and the air contribute comparably.
            const float extinction = std::log(2.0f) / distance;

            // Behind the wall in y, so its cosine there is negative and the lamp lights the air
            // without also lighting what the air is in front of.
            const osg::Vec3f lamp(0.0f, 100.0f, 20000.0f);

            // What one unit of intensity delivers at the middle of the ray, from the same windowed
            // inverse square the shader uses: an inverse square that reaches exactly zero at the
            // light's reach, because Morrowind's is a hard cutoff and clipping one leaves a ring.
            const osg::Vec3f middle(0.0f, -0.5f * distance, 0.0f);
            const float span = (lamp - middle).length();
            const float ratio = span / reach;
            const float window = 1.0f - ratio * ratio * ratio * ratio;
            const float delivered = window * window / (span * span + 1.0f);

            const auto look = [&](bool lit, bool shaded = false) {
                SceneDesc scene = makeWall();

                // A lid between the ray and the lamp, high enough to be nowhere near what the eye
                // sees and squarely across every ray the march sends up at the light.
                if (shaded)
                    scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                        .mMesh = scene.addMesh(makeSheet(40000.0f, 1000.0f), {}, {}, sQuadIndices) });

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
            // themselves — measured at 0.03 of the frame's own mean of about 0.5.
            EXPECT_GT(apart(still, upwind), 0.01) << "an eye walking against the wind sees another air";
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
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;

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
                    .mMesh = scene.addMesh(makeSheet(4000.0f, -200000.0f), {}, {}, sQuadIndices) });

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
            const float climb = 0.5f / std::sqrt(1.25f);
            const float column = std::exp(-Shaders::FOG_HEIGHT * 3.0e-4f / climb);
            const float crossed = 1.0f - std::exp(-3.0e-4f * Shaders::FOG_REACH);

            EXPECT_NEAR(ahead, irradiance * forward * column * crossed, 0.006f)
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
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;
            constexpr float irradiance = 4.0f;

            const auto look = [&](bool lidded, bool lit) {
                // The same sheet either way, over the march or under it, so the two frames differ
                // in what the shadow ray finds and in nothing else — not in what is in the scene,
                // nor in how large it is.
                SceneDesc scene;
                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                    .mMesh = scene.addMesh(makeSheet(40000.0f, lidded ? 500.0f : -500.0f), {}, {}, sQuadIndices) });

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
    }
}
