#include "fixture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Rtx::Testing
{
    namespace
    {
        /// What the centre pixel must read over `depth` units of flat water with a bed under it.
        ///
        /// The bed is untextured, so its albedo is 0.5, and **the water is crossed twice**: the sun
        /// is attenuated on its way down and again on the way back up to the eye, so what arrives is
        /// the product of two paths and not one of them. Lighting the bottom as though the water
        /// over it were not there is what makes the same column read differently from above and
        /// below.
        ///
        /// The way down is the *slant* path — Snell bends the sun to `asin(sin(zenith) / IOR)` and
        /// it crosses `depth / cos` of water — while the way back is vertical, because the camera
        /// is. The cosine on the bed stays the sun's own in air: refraction at a level surface moves
        /// no flux across a horizontal patch, so what lands on the bottom is what fell on the top
        /// times whatever the longer path took.
        ///
        /// Nothing else adds. The scattering term `(1 - T^2) / 2` meets a black ambient, and the
        /// sky is black, so all the surface reflects is the two per cent it takes off the way in.
        int throughFlatWater(float depth, float zenith, std::size_t channel)
        {
            const float sine = std::sin(zenith) / Shaders::WATER_IOR;
            const float refracted = std::sqrt(1.0f - sine * sine);

            const float down = std::exp(-Shaders::WATER_EXTINCTION[channel] * depth / refracted);
            const float up = std::exp(-Shaders::WATER_EXTINCTION[channel] * depth);
            const float bed = 0.5f * sSunOverWater * std::cos(zenith) * Shaders::INV_PI;

            return encodeSrgb(bed * down * up * (1.0f - Shaders::WATER_F0));
        }

        /// Water is seen by a camera and not by a shadow ray, and the mask is what says so.
        ///
        /// Sunlight reaching a seabed has come through the surface, so a sea that occluded would
        /// black out every shallow in the game. Telling traversal in the mask costs nothing; the
        /// alternative — non-opaque water and a candidate loop that waves shadow rays past — was
        /// measured at half the frame rate, because every shadow ray crossing the sea then invokes a
        /// shader where traversal alone had been enough.
        TEST_F(RtxVisibilityTest, waterIsVisibleToACameraAndInvisibleToAShadowRay)
        {
            const auto made = [](MaterialKind kind) {
                return [kind](SceneDesc& scene, std::span<const osg::Vec3f> pane) {
                    Material material;
                    material.mKind = kind;
                    scene.addInstance(MeshInstance{
                        .mTransform = osg::Matrixf::identity(),
                        .mMesh = scene.addMesh(pane, {}, {}, sQuadIndices),
                        .mMaterial = scene.addMaterial(material),
                    });
                };
            };

            // A solid pane stops the camera's ray and the sun's alike, so the wall behind is dark.
            EXPECT_EQ(paneOverWall(made(MaterialKind::Surface), false)[0], 0) << "a solid pane shadows the wall";

            // The same pane as water: the camera still meets it, and the sun goes straight through.
            // 0.5 albedo times 2.0 over pi is 0.318310, which encodes to 153 of 255.
            EXPECT_EQ(paneOverWall(made(MaterialKind::Water), false)[0], 153) << "and water does not";

            // And the camera does still meet it. Asserted as "bluer than it is red", which the wall
            // cannot be at any brightness — it is grey through every channel — and which survives
            // water's shading changing, as it will.
            const std::array<std::uint8_t, 3> seen = paneOverWall(made(MaterialKind::Water), true);
            EXPECT_GT(seen[2], seen[0]) << "though a camera still sees it";
        }

        /// The player's own arms are seen by the eye and shadow nothing.
        ///
        /// The same pane as the water's test, placed as first person: the patch of wall behind it
        /// reads exactly what it reads with nothing in the way, and the pane is still there to look
        /// at. A pair of hands with no body behind them would otherwise cast the shadow of a pair of
        /// hands, which the game's own casting masks never let it do.
        TEST_F(RtxVisibilityTest, theFirstPersonArmsAreSeenAndShadowNothing)
        {
            const auto placed = [](bool firstPerson) {
                return [firstPerson](SceneDesc& scene, std::span<const osg::Vec3f> pane) {
                    scene.addInstance(MeshInstance{
                        .mTransform = osg::Matrixf::identity(),
                        .mMesh = scene.addMesh(pane, {}, {}, sQuadIndices),
                        .mFirstPerson = firstPerson,
                    });
                };
            };

            const std::uint8_t open = paneOverWall([](SceneDesc&, std::span<const osg::Vec3f>) {}, false)[0];
            ASSERT_GT(open, 0) << "the sun lights the wall";
            EXPECT_EQ(paneOverWall(placed(false), false)[0], 0) << "a solid pane shadows the wall";
            EXPECT_EQ(paneOverWall(placed(true), false)[0], open) << "and the arms do not";
            EXPECT_GT(paneOverWall(placed(true), true)[0], 0) << "though the eye still sees them";
        }

        /// Water over a bed, absorbing exactly what Beer-Lambert says it should over the path taken.
        ///
        /// A flat sea, so the surface is its own plane and the ray crosses the depth once rather than
        /// through whatever facet a wave put in the way. Nearly straight down — `makeCamera` will not
        /// take a look along the world's up axis, so this is a fifth of a degree off it, where the
        /// cosine is 0.99999 and Fresnel is its head-on 0.02.
        ///
        /// **The sun's path is not the depth unless the sun is overhead**, which is the second half
        /// of this: it enters at Snell's angle and crosses more water than it would straight down.
        ///
        /// Every expectation here derives from `WATER_EXTINCTION`, so a tuning pass is one line
        /// rather than five pieces of arithmetic that quietly stop describing the shader.
        TEST_F(RtxVisibilityTest, waterTakesWhatBeerLambertSaysOverThePathTheLightTook)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);
            constexpr float depth = 200.0f;

            // A bed and a surface, both level and both wide enough to fill the frame.
            const SceneDesc scene = makeFlooded(400.0f, depth);

            const auto look = [&](float zenith) {
                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -1.0f, 400.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);
                litThroughWater(camera, zenith);

                // No height at all, which is a flat sea: a table whose amplitudes are zero. It is
                // also what makes the caustic exactly one — a flat surface has no curvature to
                // gather anything with, so the Jacobian is the identity.
                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels, { .mSea = SeaState{ .mSignificantHeight = 0.0f } });
                return std::array<int, 3>{ pixels[centre], pixels[centre + 1], pixels[centre + 2] };
            };

            // A sun as near overhead as the reflection allows, where `throughFlatWater`'s slant is
            // a part in three thousand and the answer is all but exactly `bed * T^2`:
            //
            //   0.5 * 2.0 / pi = 0.318310, times the two transmittances, times the 0.98 that is not
            //   the surface's own reflection.
            const std::array<int, 3> overhead = look(sNearlyOverhead);
            for (std::size_t channel = 0; channel < 3; ++channel)
                EXPECT_NEAR(overhead[channel], throughFlatWater(depth, sNearlyOverhead, channel), 1)
                    << "channel " << channel << " under a sun all but overhead";

            // And the ordering that makes this water's colour. Red goes first — every water absorbs
            // it within a metre or two, which is the one thing about water's colour that is not a
            // matter of taste. **Blue survives longest**, which is why a body of water reads blue
            // once it is deep enough to read as anything: water absorbs red twenty-five times as
            // fast as blue, and the dissolved matter that stains a coast blue-ward does not close
            // that. It is also what `WATER_SCATTER` says, its own peak being in blue.
            EXPECT_LT(overhead[0], overhead[1]) << "red is taken before green";
            EXPECT_LT(overhead[1], overhead[2]) << "and green before blue";

            // And now 45 degrees off the vertical, where the slant is the whole point. Snell's law
            // turns the sun to `asin(sin(45) / 1.333)` = 32.03 degrees, so it reaches a bed 200
            // units down after 200 / cos(32.03) = 235.93 units of water rather than 200 — while the
            // view back up is unchanged. One expectation serves both angles, which is what makes
            // this a measurement of the zenith rather than two unrelated numbers.
            const float slanted = osg::DegreesToRadians(45.0f);
            const std::array<int, 3> across = look(slanted);
            for (std::size_t channel = 0; channel < 3; ++channel)
                EXPECT_NEAR(across[channel], throughFlatWater(depth, slanted, channel), 1)
                    << "channel " << channel << " under a slanted sun";

            // The parameter has to matter, and it does in the direction physics says: a slanted sun
            // both meets the bed at a cosine and crosses more water to get there, so less of it
            // comes back. Red, which the extra 36 units of water costs the most.
            EXPECT_LT(across[0], overhead[0]) << "a slanted sun reaches the bed with less left";
        }

        /// Deep water settles at what it scatters, and at half what only-the-return-leg would give.
        ///
        /// Light scattered toward the eye had to get down there first. Attenuating only the way back
        /// lets deep water asymptote to the scattering colour at full sky brightness, which is a
        /// milky sheet rather than a channel. Integrating both legs replaces `1 - T` with
        /// `(1 - T^2) / 2` — the same answer in the shallows, half as bright where it settles.
        ///
        /// Two thousand units down, red's transmittance is `exp(-7.487)`, six parts in ten thousand,
        /// so what comes back is the scattering term almost alone:
        ///
        ///   0.5 * 5.61e-4 + 0.04 * 0.5 * 1.0  = 0.0202807
        ///   times the 0.98 that is not Fresnel = 0.0198751
        ///   1.055 * 0.0198751^(1/2.4) - 0.055  = 0.15118, or 39 of 255
        ///
        /// Only the return leg would put the same pixel at 56 — the factor of two, made visible.
        ///
        /// **And water with no bottom at all settles there outright**, which is the second half of
        /// this. A refraction that finds nothing went *down*, and down from the surface there is
        /// water whether or not this renderer holds the bed for it — so what comes back is the
        /// scattering term with no bed left in it at all, in every channel rather than in red
        /// alone. Two thousand units is deep for red and nothing like deep for blue.
        ///
        /// Read as sky instead, it came back through 2000 units of water at half of blue, and drew
        /// the edge of the loaded terrain across the sea as a line with brighter water beyond it.
        TEST_F(RtxVisibilityTest, deepWaterSettlesAtHalfWhatOneAttenuatedLegWouldGive)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);
            constexpr float depth = 2000.0f;

            // No sun and a black sky, so the ambient is the only light and the two per cent that
            // reflects off the surface reflects nothing.
            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -1.0f, 400.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
            camera.mAmbient = osg::Vec3f(1.0f, 1.0f, 1.0f);

            const auto look = [&](const SceneDesc& scene) {
                // No height at all, which is a flat sea: a table whose amplitudes are zero.
                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels, { .mSea = SeaState{ .mSignificantHeight = 0.0f } });
                return std::array<int, 3>{ pixels[centre], pixels[centre + 1], pixels[centre + 2] };
            };

            // `WATER_SCATTER.r * 0.5` is 0.02, less the two per cent the surface reflects away, and
            // the display curve puts 0.0199 at 39.
            const std::array<int, 3> bedded = look(makeFlooded(4000.0f, depth));
            EXPECT_EQ(bedded[0], 39) << "red, settled at what the water scatters";

            // The same column with the bed taken out from under it, which is the asymptote itself:
            // `WATER_SCATTER * 0.5`, less Fresnel. Off the constants rather than written out, for
            // the reason the expectations above are.
            const std::array<int, 3> bottomless = look(makeOpenWater(4000.0f));
            for (std::size_t channel = 0; channel < 3; ++channel)
                EXPECT_NEAR(bottomless[channel],
                    encodeSrgb(Shaders::WATER_SCATTER[channel] * 0.5f * (1.0f - Shaders::WATER_F0)), 1)
                    << "channel " << channel << " over water with no bottom";

            // And the bed at 2000 units is still there in the two channels it is not deep for, so
            // this cannot pass by the two scenes rendering the same picture.
            EXPECT_GT(bedded[2], bottomless[2] + 40) << "blue reaches a bed 2000 units down";
        }

        /// The same column of water has to look the same from either side of it.
        ///
        /// **This is the test that found the missing half.** Seen from ten units above, a ray crosses
        /// the surface, the water and back; seen from ten units below, it crosses the water and the
        /// ray is fogged by it directly. Those are different code paths through the same physics, and
        /// they have to agree — which they cannot while the light reaching the bottom is not
        /// attenuated by the water over it.
        ///
        /// M6's stated done-when, and the sort of thing that is found by measuring rather than by
        /// looking: neither view is obviously wrong on its own.
        TEST_F(RtxVisibilityTest, aColumnOfWaterAgreesFromAboveAndFromBelow)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);
            constexpr float depth = 200.0f;

            const SceneDesc scene = makeFlooded(400.0f, depth);

            // A fifth of a degree off the vertical, which `makeCamera` insists on and which changes
            // the path by a part in ten thousand.
            const auto look = [&](float from) {
                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -0.05f, from), osg::Vec3f(0.0f, 0.0f, from - 10.0f), 60.0f, size, size, 10000.0f);
                litThroughWater(camera);

                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels, { .mSea = SeaState{ .mSignificantHeight = 0.0f } });
                return std::array<int, 3>{ pixels[centre], pixels[centre + 1], pixels[centre + 2] };
            };

            // Ten above and ten below, and the two legitimately differ by a little. From above the
            // ray crosses the whole 200 units after the surface and loses two per cent to Fresnel on
            // the way in; from below it crosses 190 and meets no surface at all. In radiance that is
            //
            //   exp(0.003743 * 10) / 0.98 = 1.059
            //
            // for red, which the sRGB curve compresses to under three per cent of a byte — so 75 and
            // 77, and a tolerance of two rather than a round fraction chosen to pass.
            const std::array<int, 3> above = look(10.0f);
            const std::array<int, 3> below = look(-10.0f);

            EXPECT_GT(above[0], 0) << "the bed is visible from above";
            for (std::size_t channel = 0; channel < 3; ++channel)
                EXPECT_NEAR(below[channel], above[channel], 2) << "channel " << channel << ": " << above[channel]
                                                               << " from above, " << below[channel] << " from below";
        }

        /// The water *over* an eye dims what the water in front of it scatters.
        ///
        /// **The half that only a submerged camera can see.** `waterColumn` charges the sun for the
        /// water between the surface and where the stretch begins, and the ambient for nothing at
        /// all — so the sea's own scattering arrived at full sky brightness however deep the eye
        /// was. From above that is right, because the stretch begins at the surface and there is no
        /// water over it. From below it is the whole column over the camera, missing.
        ///
        /// **The stretch is held at 200 units and the bed moves with the eye**, so the only thing
        /// that changes between the two legs is how much water stands over them. **And the bed sends
        /// back nothing**, which is what leaves the scattering alone to be measured: an eye ray's
        /// terminator is nought and the bounce it traces instead finds a black sky, so what reaches
        /// the pixel is what the column put there. That is
        ///
        ///   w (1 - exp(-2 o 200)) / 2 * exp(-o eye)
        ///
        /// Charge the ambient nothing and the last factor is gone, which is what this measures: 33
        /// and 22 at both depths alike, where a thousand units of water over the eye should have
        /// taken red to nothing and blue to three quarters.
        TEST_F(RtxVisibilityTest, theWaterOverAnEyeDimsWhatTheWaterInFrontOfItScatters)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);
            constexpr float stretch = 200.0f;

            // No sun and a black sky, so the ambient is the only light and the answer is the two
            // exponentials. The bed is untextured, which is an albedo of a half.
            const auto look = [&](float eye) {
                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -0.05f, -eye), osg::Vec3f(0.0f, 0.0f, -eye - 10.0f), 60.0f, size, size, 10000.0f);
                camera.mAmbient = osg::Vec3f(1.0f, 1.0f, 1.0f);
                camera.mAmbientFromSky = 1.0f;
                camera.mWaterLevel = 0.0f;

                std::vector<std::uint8_t> pixels;
                const SceneDesc scene = makeFlooded(4000.0f, eye + stretch);
                countHits(scene, {}, camera, size, pixels, { .mSea = SeaState{ .mSignificantHeight = 0.0f } });
                return std::array<int, 3>{ pixels[centre], pixels[centre + 1], pixels[centre + 2] };
            };

            const auto scattered = [&](float eye, std::size_t channel) {
                const float o = Shaders::WATER_EXTINCTION[channel];

                return encodeSrgb(Shaders::WATER_SCATTER[channel] * 0.5f * (1.0f - std::exp(-2.0f * o * stretch))
                    * std::exp(-o * eye));
            };

            // Red and blue, which is the whole spread of what water does: red is gone by a thousand
            // units and blue keeps three quarters of itself, so neither a saturated channel nor an
            // untouched one can carry this alone.
            for (const float eye : { 10.0f, 1000.0f })
            {
                const std::array<int, 3> seen = look(eye);
                EXPECT_NEAR(seen[0], scattered(eye, 0), 1) << "red, " << eye << " units under";
                EXPECT_NEAR(seen[2], scattered(eye, 2), 1) << "blue, " << eye << " units under";
            }
        }

        /// A ray that goes down from under the surface and finds nothing is water, not sky.
        ///
        /// **The plane has absolute sides.** Below it there is water, whether or not this renderer
        /// was handed a bed far enough out to stop the ray — and `mFar` is a setting a camera
        /// carries rather than a distance the sea has. Read as sky, everything past the edge of the
        /// loaded terrain arrived as the sky's own colour through `mFar` of water instead of through
        /// all of it, which drew the terrain's boundary across the sea as a row of dark panels
        /// standing along the horizon.
        ///
        /// **Open water with no bed under it and a white sky**, which puts the two answers as far
        /// apart as they go. No sun, so nothing is marched and the column is its closed form: at a
        /// hundred units down that is
        ///
        ///   scatter * (1 - T^2) / 2 * ambient * exp(-o * 100)
        ///
        /// with `T` the transmittance over the whole column, which is nought in every channel — so
        /// the first factor is the asymptote `deepWaterSettlesAtHalfWhatOneAttenuatedLegWouldGive`
        /// measures from over the surface, dimmed here by the water above the eye and crossing no
        /// surface to lose its Fresnel share. That is 31, 61 and 69 of 255. Against a far plane at
        /// two thousand units, blue keeps half of itself and the sky behind it reads 195, three
        /// quarters of the scale away.
        ///
        /// **And the same byte over the whole frame**, which is the half that was visible: what the
        /// column settles at depends on neither the ray nor the far plane, so nothing here can draw
        /// an edge across the water.
        TEST_F(RtxVisibilityTest, aRayThatFindsNothingUnderTheSurfaceIsWaterRatherThanSky)
        {
            constexpr std::uint32_t size = 33;
            constexpr float eye = 100.0f;

            // A far plane short enough that blue would carry half the sky through it, so a test
            // passing this cannot be one the extinction happened to swallow.
            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -0.05f, -eye), osg::Vec3f(0.0f, 0.0f, -eye - 10.0f), 60.0f, size, size, 2000.0f);
            camera.mAmbient = osg::Vec3f(1.0f, 1.0f, 1.0f);
            camera.mSkyHorizon = osg::Vec3f(1.0f, 1.0f, 1.0f);
            camera.mSkyZenith = camera.mSkyHorizon;
            camera.mAmbientFromSky = 1.0f;
            camera.mWaterLevel = 0.0f;

            std::vector<std::uint8_t> pixels;
            EXPECT_EQ(countHits(makeOpenWater(4000.0f), {}, camera, size, pixels), 0u)
                << "the sheet is overhead, so every ray leaves the scene";

            std::array<int, 3> lowest{ 255, 255, 255 };
            std::array<int, 3> highest{ 0, 0, 0 };
            for (std::size_t at = 0; at < pixels.size(); at += 4)
                for (std::size_t channel = 0; channel < 3; ++channel)
                {
                    lowest[channel] = std::min(lowest[channel], int{ pixels[at + channel] });
                    highest[channel] = std::max(highest[channel], int{ pixels[at + channel] });
                }

            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                const float settled
                    = Shaders::WATER_SCATTER[channel] * 0.5f * std::exp(-Shaders::WATER_EXTINCTION[channel] * eye);

                EXPECT_NEAR(lowest[channel], encodeSrgb(settled), 1) << "channel " << channel;
                EXPECT_EQ(lowest[channel], highest[channel]) << "channel " << channel << " draws an edge";
            }
        }

        /// A pixel of water with no water under it is the ground beside it, and how much water is
        /// under it is measured straight down.
        ///
        /// **Along the refraction was the wrong measure, and a grazing view of a shore is where it
        /// showed.** The refracted ray leaves the surface at forty degrees off the vertical and
        /// lands somewhere else entirely — at a shore, somewhere the bed is much further down — so
        /// the fade read deep water at a pixel with none, never engaged, and left the plane cutting
        /// the terrain along a hard line. Straight down is a view-independent answer.
        ///
        /// Two parallel projections of one shore, one straight down and one sixty degrees off it,
        /// with their rows laid over the same run of x. The bed is black under a white sky and
        /// there is no sun, so the only thing water changes about a pixel is the sky it reflects —
        /// a constant across a parallel view — and what a row measures is the fade itself. The bed
        /// falls one in ten, so the band's middle is 17.5 units of depth and x = 175 in both views.
        /// Along the refraction the grazing view read 1.44 times the depth and put the middle 53
        /// units nearer the line, which is four rows of this frame.
        TEST_F(RtxVisibilityTest, theWaterlineIsAsDeepAsTheWaterOverItAndNotAsFarAsARayThroughItGoes)
        {
            constexpr std::uint32_t size = 64;
            constexpr float extent = 4000.0f;
            constexpr float slope = 0.1f;
            constexpr float centre = 200.0f;
            constexpr float span = 800.0f;
            constexpr float rowUnits = span / static_cast<float>(size);

            // A bed that rises through the water along +x, with the waterline at x = 0: the same
            // corner order as `sheetAt`, so it faces up.
            const std::array<osg::Vec3f, 4> bed{
                osg::Vec3f(-extent, -extent, extent * slope),
                osg::Vec3f(extent, -extent, -extent * slope),
                osg::Vec3f(extent, extent, -extent * slope),
                osg::Vec3f(-extent, extent, extent * slope),
            };

            Material black;
            black.mDiffuseColour = osg::Vec4f(0.0f, 0.0f, 0.0f, 1.0f);

            SceneDesc dry;
            dry.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                .mMesh = dry.addMesh(bed, {}, {}, sQuadIndices),
                .mMaterial = dry.addMaterial(black) });

            SceneDesc wet = makeOpenWater(extent);
            wet.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                .mMesh = wet.addMesh(bed, {}, {}, sQuadIndices),
                .mMaterial = wet.addMaterial(black) });

            // Straight down, with the image's up along +x so that a row is a run of x.
            const osg::Matrixf above = osg::Matrixf::lookAt(
                osg::Vec3f(centre, 0.0f, 500.0f), osg::Vec3f(centre, 0.0f, 0.0f), osg::Vec3f(1.0f, 0.0f, 0.0f));

            // Sixty degrees off the vertical, along +x. A parallel ray `v` up the image lands on the
            // plane at `eye.x + eye.z tan(60) + 2v`, so half the span in image units covers the same
            // run of x as the view from above.
            const float tilt = osg::DegreesToRadians(60.0f);
            const osg::Vec3f forward(std::sin(tilt), 0.0f, -std::cos(tilt));
            const osg::Vec3f eye(centre - 500.0f * std::tan(tilt), 0.0f, 500.0f);
            const osg::Matrixf grazing = osg::Matrixf::lookAt(eye, eye + forward, osg::Vec3f(0.0f, 0.0f, 1.0f));

            // Where the fade first reaches half of its deep value, against the waterline it belongs
            // at. Two rows either side, which is twenty-five units of x and two and a half of depth.
            //
            // **The comparison is made here rather than by the caller**, so that a run which never
            // reaches half reports that and nothing else: a lambda returning a figure it could not
            // find has to answer with something, and whatever it answers fails the caller's
            // comparison a second time for a reason that is not the fault.
            const auto expectTheMiddleAtTheWaterline = [&](const osg::Matrixf& view, float worldHeight,
                                                           const char* which) {
                Shaders::VisibilityConstants camera
                    = makeOrthographicCameraFromView(view, span, worldHeight, size, size, 5.0f, 20000.0f);
                camera.mWaterLevel = 0.0f;
                camera.mSkyHorizon = osg::Vec3f(1.0f, 1.0f, 1.0f);
                camera.mSkyZenith = osg::Vec3f(1.0f, 1.0f, 1.0f);
                camera.mAmbientFromSky = 1.0f;

                std::vector<std::uint8_t> withWater;
                countHits(wet, {}, camera, size, withWater, { .mSea = SeaState{ .mSignificantHeight = 0.0f } });
                std::vector<std::uint8_t> without;
                countHits(dry, {}, camera, size, without, { .mSea = SeaState{ .mSignificantHeight = 0.0f } });

                // How far each row is from the ground beside it, as the mean over its columns.
                std::vector<double> apart(size, 0.0);
                for (std::uint32_t row = 0; row < size; ++row)
                {
                    for (std::uint32_t column = 0; column < size; ++column)
                    {
                        const std::size_t i = (std::size_t{ row } * size + column) * 4;
                        apart[row] += std::abs(double{ decodeSrgb(withWater[i]) } - double{ decodeSrgb(without[i]) });
                    }
                    apart[row] /= static_cast<double>(size);
                }

                // Row zero is the top of the image, which is the far end of +x.
                const auto xAt = [&](std::uint32_t row) {
                    return centre
                        + (1.0f - 2.0f * (static_cast<float>(row) + 0.5f) / static_cast<float>(size)) * span * 0.5f;
                };

                // Deep water is the reference: fifty units and more of it, where the fade is one.
                double deep = 0.0;
                std::uint32_t deepRows = 0;
                for (std::uint32_t row = 0; row < size; ++row)
                    if (xAt(row) * slope >= 50.0f)
                    {
                        deep += apart[row];
                        ++deepRows;
                    }
                deep /= static_cast<double>(deepRows);
                EXPECT_GT(deep, 0.01) << which << ": deep water reflects a sky the black bed does not";

                // The dry side is the ground either way.
                for (std::uint32_t row = 0; row < size; ++row)
                {
                    if (xAt(row) < -rowUnits)
                    {
                        EXPECT_LT(apart[row], 0.01) << which << ": row " << row << " is dry";
                    }
                }

                // From the waterline toward deep water, where the difference first reaches half.
                for (std::uint32_t row = size; row-- > 0;)
                    if (xAt(row) > 0.0f && apart[row] >= 0.5 * deep)
                    {
                        EXPECT_NEAR(xAt(row), 175.0f, 2.0f * rowUnits) << which;
                        return;
                    }

                ADD_FAILURE() << which << ": the water never reached half of its depth";
            };

            expectTheMiddleAtTheWaterline(above, span, "from above");
            expectTheMiddleAtTheWaterline(grazing, span * 0.5f, "at sixty degrees");
        }

        /// The water between an eye and the surface over it is water like any other.
        ///
        /// **The half the invariant above cannot see.** Both of its cameras look *down*, so the hit
        /// is the bed and the column is charged whichever side the eye is on. Aim up from below and
        /// the hit is the surface itself, which used to be excluded outright — the reasoning being
        /// that a ray reaching it from below had already paid — and what had paid was each of the
        /// surface's own rays over its own stretch. The stretch from the eye up to the surface is a
        /// different one, and nothing was charging it: a surface seen from a hundred units down read
        /// exactly as bright as one seen from ten.
        ///
        /// **No sun and no ambient, so nothing scatters into the ray and the answer is the exponent
        /// alone.** The bed is then unlit and what reflects off the underside of the surface is
        /// black, which leaves the sky through Snell's window as the whole of what the surface
        /// sends down. Looking straight up, that is
        ///
        ///   surface = 0.5 sky * (1 - 0.02 Fresnel)  = 0.49
        ///   at 100  = 0.49 * exp(-0.003743 * 100)   = 0.337,  or 157 of 255
        ///   at 300  = 0.49 * exp(-0.003743 * 300)   = 0.159,  or 111
        ///
        /// Red, because water takes it out four times faster than green. Charge the eye's own
        /// stretch nothing and both read 157 — a fifth of the scale apart, so this cannot be passed
        /// by widening a tolerance.
        TEST_F(RtxVisibilityTest, theWaterBetweenAnEyeAndTheSurfaceOverItIsChargedForToo)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);

            const SceneDesc scene = makeFlooded(4000.0f, 2000.0f);

            const auto lookUp = [&](float from) {
                // A fifth of a degree off the vertical, which `makeCamera` insists on and which
                // leaves the ray well inside Snell's window.
                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -0.05f, from), osg::Vec3f(0.0f, 0.0f, from + 10.0f), 60.0f, size, size, 10000.0f);

                camera.mWaterLevel = 0.0f;
                camera.mSkyHorizon = osg::Vec3f(0.5f, 0.0f, 0.0f);
                camera.mSkyZenith = camera.mSkyHorizon;
                camera.mAmbientFromSky = 1.0f;

                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels, { .mSea = SeaState{ .mSignificantHeight = 0.0f } });
                return static_cast<int>(pixels[centre]);
            };

            EXPECT_NEAR(lookUp(-100.0f), 157, 2) << "a hundred units of water over the eye";
            EXPECT_NEAR(lookUp(-300.0f), 111, 2) << "and three hundred take three times as much red";
        }

        /// The water scatters the sun forward far harder than back, and the ratio is the whole test.
        ///
        /// **Water is not fog.** Petzold's coastal particles have a mean cosine of 0.92 against the
        /// fog's droplets, so an underwater haze is a beam around the sun rather than an even
        /// milkiness. Before this the water scattered only the sky, so facing the sun under it
        /// rendered identically to facing away.
        ///
        /// **Two horizontal rays out of the same eye, and everything but the phase cancels.** Both
        /// run until the water runs out and both come back with a black sky, so the frame is the
        /// medium alone; both are level, so the geometry term `1 - k d.z` is one for each; and both
        /// leave the same depth, so the sun's own way down is the same. What is left is
        ///
        ///   forward   HG(0.92,  0.705) = 0.030040
        ///   backward  HG(0.92, -0.705) = 0.002194
        ///
        /// a ratio of 13.7. The cosines are the refraction of a sun 70 degrees off the vertical,
        /// which Snell bends to 45 — the widest a sun ever reaches under water, and why the beam is
        /// there at all rather than only for a sun overhead.
        TEST_F(RtxVisibilityTest, theWaterScattersTheSunForwardFarHarderThanBack)
        {
            constexpr std::uint32_t size = 32;
            constexpr std::size_t centre = centreValueOf(size);

            const SceneDesc scene = makeFlooded(4000.0f, 2000.0f);

            const auto along = [&](float sign) {
                Shaders::VisibilityConstants camera = makeCamera(osg::Vec3f(0.0f, 0.0f, -1000.0f),
                    osg::Vec3f(0.0f, sign * 1000.0f, -1000.0f), 60.0f, size, size, 100000.0f);

                camera.mWaterLevel = 0.0f;
                camera.mSunPosition = sunStandingAt(osg::DegreesToRadians(70.0f));

                // A hundred times the sun the other water tests use. What the water scatters
                // sideways out of a beam is a fraction of a per cent of it, and at the usual
                // brightness both frames land in the first ten values of a byte — where the
                // quantisation is the measurement rather than the phase function.
                constexpr float blazing = 100.0f * sSunOverWater;
                camera.mSunIrradiance = osg::Vec3f(blazing, blazing, blazing);

                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels, { .mSea = SeaState{ .mSignificantHeight = 0.0f } });

                return double{ decodeSrgb(pixels[centre + 1]) };
            };

            // **The centre pixel and not the frame's mean.** A lobe this narrow is most of its own
            // integral within a few degrees, so a mean over sixty of them measures the field of view
            // rather than the phase function. Green, which is what the water scatters most of.
            const double forward = along(-1.0f);
            const double backward = along(1.0f);

            EXPECT_GT(forward, 0.0) << "the water is lit by the sun at all";
            EXPECT_NEAR(forward / backward, 13.7, 1.5) << forward << " toward the sun, " << backward << " away";
        }

        /// The sky loses the column of water over a bed just as the sun does.
        ///
        /// **The half a bounce could quietly skip.** A bounce that escapes to the sky is the sky
        /// arriving at the point it left, so it crosses the same depth the sun crosses and has to
        /// lose the same fraction to it. Returning the sky whole lights a submerged floor as though
        /// the water above it were not there — the fault `aColumnOfWaterAgreesFromAboveAndFromBelow`
        /// was written for, in the one term that test cannot see: a bounce shades the same bed
        /// identically from either side of the surface, so the two views agree while both are wrong.
        ///
        /// **From under the water, because only a primary hit bounces.** A bed seen down through the
        /// surface is the far end of `waterRay`, which terminates it with `pathEnd` — the flat
        /// ambient — precisely so that a reflection cannot recurse. Put the eye below and the bed is
        /// what the camera ray found, and it gathers a real hemisphere.
        ///
        /// **A sky of one radiance, which makes that hemisphere exact rather than noisy.** Every
        /// direction returns the same number, so the single sample carries no variance and what
        /// follows is an equality rather than an average.
        ///
        /// Red, off a bed two hundred units down, seen from a hundred and ninety units above it:
        ///
        ///   down  = exp(-0.004572 * 200)         = 0.400757   the sky's own way in
        ///   bed   = 0.5 albedo * 0.6 sky * down  = 0.120227   what leaves it
        ///   out   = exp(-0.004572 * 190)         = 0.419505   and the way back to the eye
        ///   pixel = bed * out                    = 0.050436,  or 63 of 255
        ///
        /// No Fresnel and no surface: the ray never crosses one. Green and blue absorb far less and
        /// land at 131 and 121. Drop the column and red reads 99 — half the scale away, so this
        /// cannot be passed by widening a tolerance.
        TEST_F(RtxVisibilityTest, aBounceIntoTheSkyLosesTheWaterOverTheBedItLeft)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);
            constexpr float depth = 200.0f;
            constexpr float above = 190.0f;
            constexpr float sky = 0.6f;

            const SceneDesc scene = makeFlooded(4000.0f, depth);

            // Straight down but for the third of a degree `makeCamera` insists on — it refuses a
            // view along the world's up axis, where roll has no answer — so the way out is the
            // vertical column to a part in seventy thousand and the bed is met square on.
            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -1.0f, above - depth), osg::Vec3f(0.0f, 0.0f, -depth), 60.0f, size, size, 10000.0f);
            camera.mSkyHorizon = osg::Vec3f(sky, sky, sky);
            camera.mSkyZenith = camera.mSkyHorizon;
            camera.mAmbientFromSky = 1.0f;
            camera.mWaterLevel = 0.0f;

            // Flat, so the surface the bounce passes through neither bends it nor gathers it.
            std::vector<std::uint8_t> pixels;
            countHits(scene, {}, camera, size, pixels, { .mSea = SeaState{ .mSignificantHeight = 0.0f } });

            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                const float down = std::exp(-Shaders::WATER_EXTINCTION[channel] * depth);
                const float out = std::exp(-Shaders::WATER_EXTINCTION[channel] * above);

                EXPECT_NEAR(pixels[centre + channel], int{ encodeSrgb(0.5f * sky * down * out) }, 1)
                    << "channel " << channel;
            }
        }

        /// A reflection is not where the water is, and reprojects from where its image is.
        ///
        /// **The one surface in the frame that shows something standing somewhere else.** Water is
        /// shaded on the primary hit, so the ordinary motion vector describes the water — and a
        /// shoreline mirrored in a lake then swims as the camera walks, because it is being
        /// reprojected at the depth of the surface carrying it rather than at the depth of its own
        /// image.
        TEST_F(RtxVisibilityTest, aReflectionReprojectsFromWhereItsImageIsAndNotFromTheWater)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreOf(size);

            // A ceiling two hundred units over the water, with the eye a hundred up between them
            // looking straight down. The water under the eye is a hundred away; the ceiling's image
            // in the plane is at `z = -200`, which is three hundred — so a step sideways moves the
            // reflection exactly a third of what it moves the surface, and only a mirrored
            // reprojection can produce that. Reprojecting the ceiling where it actually stands would
            // put it behind the camera.
            constexpr float step = 10.0f;
            constexpr float halfHeight = 0.5773503f;
            constexpr float surface = size * step / (2.0f * 100.0f * halfHeight);
            constexpr float image = size * step / (2.0f * 300.0f * halfHeight);

            SceneDesc scene = makeOpenWater(4000.0f);
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                .mMesh = scene.addMesh(sheetAt(4000.0f, 200.0f), {}, {}, sQuadIndices) });

            const auto look = [&](float across) {
                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(across, -1.0f, 100.0f), osg::Vec3f(across, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);

                // The plane the water geometry lies in, which the mirrored reprojection reflects
                // about. A scene with water in it and no level named is not one this renderer makes.
                camera.mWaterLevel = 0.0f;
                return camera;
            };

            // **A flat sea, because this is a claim about the plane.** The image of a point in a
            // tilted facet is not its image in the plane, so a wave scatters the reflection by
            // exactly the amount this reprojection cannot describe — which is what the bias mask
            // beside it is for, and not what is being measured here.
            constexpr SeaState still{ .mSignificantHeight = 0.0f };

            std::vector<std::uint8_t> pixels;
            countHits(scene, {}, look(0.0f), size, pixels, { .mSea = still });

            std::vector<float> mirrored;
            std::vector<float> moved;
            mRenderer->readChannel(Channel::ReflectionMotion, mirrored);
            mRenderer->readChannel(Channel::Motion, moved);
            ASSERT_EQ(mirrored.size(), std::size_t{ size } * size * 2);

            // **Nothing has moved yet**, which the mirrored field has to say as plainly as the
            // ordinary one: a still camera over still water reflects a still ceiling.
            mRenderer->renderFrame(look(0.0f), FrameOptions{});
            mRenderer->readChannel(Channel::ReflectionMotion, mirrored);
            EXPECT_NEAR(mirrored[centre * 2], 0.0f, 0.01f) << "a still frame reflects a still image";
            EXPECT_NEAR(mirrored[centre * 2 + 1], 0.0f, 0.01f);

            // A step sideways, with everything in the world standing still.
            mRenderer->renderFrame(look(step), FrameOptions{});
            mRenderer->readChannel(Channel::ReflectionMotion, mirrored);
            mRenderer->readChannel(Channel::Motion, moved);

            EXPECT_NEAR(std::abs(moved[centre * 2]), surface, 0.1f) << "the water is a hundred units under the eye";
            EXPECT_NEAR(std::abs(mirrored[centre * 2]), image, 0.1f)
                << "and what it reflects has its image three hundred under, so it moves a third as far";
            EXPECT_LT(std::abs(mirrored[centre * 2]), std::abs(moved[centre * 2]))
                << "which is the whole point: the two are not the same vector";

            // **And the sky reflected is a reflection too.** Take the ceiling away and every one of
            // these rays reaches the sky instead, which has no distance and still moves when the
            // camera turns — writing nought there says the mirrored horizon is nailed to the screen.
            const SceneDesc bare = makeOpenWater(4000.0f);
            const auto turn = [&](float sideways) {
                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -1.0f, 100.0f), osg::Vec3f(sideways, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
                camera.mWaterLevel = 0.0f;
                return camera;
            };

            countHits(bare, {}, turn(0.0f), size, pixels, { .mSea = still });
            mRenderer->renderFrame(turn(40.0f), FrameOptions{});
            mRenderer->readChannel(Channel::ReflectionMotion, mirrored);

            EXPECT_GT(std::abs(mirrored[centre * 2]), 1.0f) << "the mirrored sky slid when the camera turned";
            EXPECT_LT(std::abs(mirrored[centre * 2]), static_cast<float>(size)) << "and stayed on screen";
        }
    }
}
