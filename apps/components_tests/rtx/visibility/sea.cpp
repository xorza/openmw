#include "fixture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Rtx::Testing
{
    namespace
    {
        /// Which level of `makeMipLadder` a linear sample came from.
        float ladderLevel(float sampled)
        {
            return (sampled * 255.0f - 40.0f) / 30.0f;
        }

        float meanOf(const std::vector<float>& field)
        {
            float total = 0.0f;
            for (const float value : field)
                total += value;

            return total / static_cast<float>(field.size());
        }

        /// How far a field varies, as a fraction of its own mean.
        ///
        /// **The measure every water pattern here is judged by**, because what a caustic or a shaft
        /// is asked to have is structure rather than brightness — and the brightness is what a
        /// ratio against a flat sea has already divided out.
        float contrastOf(const std::vector<float>& field)
        {
            const float mean = meanOf(field);

            float spread = 0.0f;
            for (const float value : field)
                spread += (value - mean) * (value - mean);

            return std::sqrt(spread / static_cast<float>(field.size())) / mean;
        }

        /// The lobe the shader arrives at for a sea read through a cone this wide.
        ///
        /// **What the chain lost at that footprint, and not the whole spectrum.** The shader reads
        /// `mLostSlope` off the level `waveLevel` picks, so what it carries is the slope a mip of
        /// that width averaged away — never all of it unless the cone reaches the coarsest level.
        /// The reason is that this is the shader's own question, and not the size of the answer: at
        /// the footprint `waterTooFineToResolveWidensTheConeItRefractsThrough` reads, the mip has
        /// lost nearly all of the slope anyway and the whole spectrum would predict 1.434 against
        /// the chain's 1.425.
        ///
        /// The lobe is twice its root: a normal tilted by an angle turns a reflection by twice it.
        float lobeOf(const SeaState& sea, float footprint)
        {
            float unresolved = 0.0f;
            for (const WaveCascade& cascade : makeWaveCascades(sea))
                unresolved += Testing::lostSlopeOf(cascade, footprint);

            return std::min(2.0f * std::sqrt(unresolved), 1.0f);
        }

        /// A shaft is the surface's own lens carried along the ray, and nothing else.
        ///
        /// **What the closed form cannot have.** `waterColumn` integrates the sun's beam exactly, and
        /// an exact integral of a smooth thing is smooth: the water brightens toward the sun and goes
        /// dark away from it, with no structure anywhere in between. A beam of sunlight in water has
        /// structure because the surface over it is a lens, and carrying that down the ray is what
        /// this measures.
        ///
        /// **A black sky and no ambient, so the beam is the whole pixel.** Looking up from deep
        /// water, the surface reflects an unlit bed and refracts a black sky, so it sends down
        /// nothing; the column's own sky term meets an ambient of nothing. What is left is the sun
        /// scattered toward the eye over fifteen hundred units — the shaft, alone.
        ///
        /// **Two seas, and the mean is what says it is light rather than decoration.** A flat sea is
        /// a lens of no power and its caustic is exactly one everywhere, so it renders the closed
        /// form; a real one redistributes that same light. The pattern is the difference between
        /// them and the total is not.
        TEST_F(RtxVisibilityTest, theSunsBeamUnderWaterCarriesTheSurfacesPattern)
        {
            constexpr std::uint32_t size = 48;
            constexpr std::size_t count = std::size_t{ size } * size;

            const SceneDesc scene = makeFlooded(4000.0f, 2000.0f);

            // Seven metres under a sun all but overhead, looking all but straight up at it — well
            // inside the share `WATER_SHAFT_FLOOR` asks for, and at a depth a shaft happens at. Twenty
            // metres down there is little pattern left in the water to carry.
            const auto lookUp = [&](const SeaState& sea) {
                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -0.05f, -500.0f), osg::Vec3f(0.0f, 0.0f, -490.0f), 60.0f, size, size, 10000.0f);
                litThroughWater(camera);

                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels, { .mSea = sea });

                std::vector<float> field;
                field.reserve(count);
                for (std::size_t i = 0; i < count; ++i)
                    field.push_back(decodeSrgb(pixels[i * 4 + 1]));

                return field;
            };

            const std::vector<float> flat = lookUp(SeaState{ .mSignificantHeight = 0.0f });
            const std::vector<float> running = lookUp(SeaState{});

            std::vector<float> ratio;
            ratio.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
                ratio.push_back(running[i] / flat[i]);

            // **The ratio, because the phase function is eighty times brighter down the sun's line
            // than at the edge of a sixty-degree frame.** That is real and it is not what is being
            // measured: dividing the two frames cancels it, along with the extinction and the
            // geometry, and leaves the lens alone. A flat sea has a lens of no power, so its own
            // caustic is exactly one everywhere and the ratio would be a field of ones.
            EXPECT_GT(contrastOf(ratio), 0.03f) << "measured 0.057, where a flat sea gives nought";
            EXPECT_NEAR(meanOf(ratio), 1.0f, 0.06f) << "measured 0.963: the shaft moves light and makes none";
        }

        /// A shaft is blocked by what stands over the water, and the gap is where the sun enters.
        ///
        /// **The half the closed form has no way to ask.** A submerged surface is shadowed because
        /// `shadeSurface` traces its own ray, and water carries a mask bit that keeps it out of
        /// occlusion — so a rock over the sea darkened the bed under it and left the water in front
        /// of the bed as bright as ever. The march is where the volume gets the same question.
        ///
        /// **Three lids, and the last is the one that says where the shadow belongs.** A lid over
        /// everything only proves that something is being asked, and a strip whose shadow falls
        /// where the light entered only proves that the answer can be partial. The last stands over
        /// that same water and casts its shadow elsewhere, so a shader that looked straight up would
        /// find it squarely in the way and darken a shaft nothing is shading.
        ///
        /// The sun is 45 degrees off the vertical, which Snell bends to 32 under the water, so from
        /// 500 units down the light met the surface 312 units up-sun of the eye — and a lid 500
        /// units over the sea shadows the water 500 units down-sun of itself. The two displacements
        /// are what the second lid is placed against, and getting either backwards puts its shadow
        /// somewhere the frame cannot see.
        ///
        /// Nothing else is in the frame: no bed, a black sky, no ambient and no disc, so the surface
        /// reflects unlit water and refracts an unlit sky. What is left is the shaft alone.
        TEST_F(RtxVisibilityTest, aShaftIsBlockedByWhatStandsOverTheWaterWhereTheSunEnters)
        {
            constexpr std::uint32_t size = 48;
            constexpr std::size_t count = std::size_t{ size } * size;
            constexpr float eye = 500.0f;

            // Straight up from under the surface, which is where the phase function puts the beam.
            // A hair off the vertical, which `makeCamera` insists on.
            const auto shaft = [&](const std::optional<std::array<osg::Vec3f, 4>>& lid) {
                SceneDesc scene = makeOpenWater(4000.0f);
                if (lid.has_value())
                    scene.addInstance(MeshInstance{
                        .mTransform = osg::Matrixf::identity(), .mMesh = scene.addMesh(*lid, {}, {}, sQuadIndices) });

                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -0.05f, -eye), osg::Vec3f(0.0f, 0.0f, -eye + 10.0f), 60.0f, size, size, 10000.0f);
                litThroughWater(camera, osg::DegreesToRadians(45.0f));

                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels);

                float total = 0.0f;
                for (std::size_t i = 0; i < count; ++i)
                    total += mRadiance[i * 4 + 1];

                return total / static_cast<float>(count);
            };

            // Well clear of the water, and wider than anything the frame reaches.
            constexpr float over = 500.0f;
            const std::array<osg::Vec3f, 4> everything = makeSheet(4000.0f, over);

            // A ray leaving the water toward a sun 45 degrees over passes this height 500 units
            // up-sun of where it started, so a lid here shadows the water 500 units down-sun of
            // itself. Up-sun is toward -y: that is where the sun stands, and its light travels +y.
            const auto strip = [](float from, float to) {
                return std::array<osg::Vec3f, 4>{
                    osg::Vec3f(-4000.0f, from, over),
                    osg::Vec3f(4000.0f, from, over),
                    osg::Vec3f(4000.0f, to, over),
                    osg::Vec3f(-4000.0f, to, over),
                };
            };

            // Its shadow lands on the water between 320 and 150 units up-sun of the eye's column,
            // which is where the light entered for the deeper half of the march.
            const std::array<osg::Vec3f, 4> shadowing = strip(-820.0f, -650.0f);

            // The same strip of water, with the lid over it rather than over its light. Nothing here
            // shadows anything the frame can see, and a shader that looked straight up would find
            // this in the way.
            const std::array<osg::Vec3f, 4> overhead = strip(-320.0f, -150.0f);

            const float open = shaft(std::nullopt);
            const float covered = shaft(everything);
            const float shaded = shaft(shadowing);
            const float beside = shaft(overhead);

            // Measured 0.0182 open and 2.7e-7 under the lid. The beam is the whole of the frame, so
            // taking the sun off the water takes the frame with it.
            EXPECT_GT(open, 0.0f) << "there is a shaft to block";
            EXPECT_LT(covered, 1.0e-4f * open) << "and a lid over all of the water puts it out";

            // **A strip takes nearly all of it and not half, because the beam is a narrow lobe.**
            // Henyey-Greenstein at 0.92 puts the shaft in the pixels looking up-sun, and a ray that
            // climbs toward the sun gains y as fast as its own entry point does — so its whole march
            // enters the water through a stretch a couple of tens of units long, and one strip
            // covers it. The pixels the strip misses are the ones that were dark anyway.
            EXPECT_NEAR(shaded / open, 0.051f, 0.02f) << "a lid whose shadow falls on the entries";

            // **The one that says where the gap belongs**, and it is 0.9991 of the open shaft.
            // Standing over the same water and casting its shadow elsewhere, it changes nothing.
            EXPECT_NEAR(beside / open, 1.0f, 0.01f) << "and one that merely stands over them";
        }

        /// `causticGain` is the mean it says it is, against the field it was fitted to.
        ///
        /// **The fit is the one number in the caustic nobody can read off the shader.** Everything
        /// else there is arithmetic or a dial; this is three coefficients standing for four million
        /// draws, and a fit nobody can check is a magic number. So the draws are made again here.
        ///
        /// The Hessian of an isotropic Gaussian field has one free parameter. Its fourth spectral
        /// moments give `Var[Hxx] = Var[Hyy] = 3c`, `Var[Hxy] = Cov[Hxx, Hyy] = c`, so
        /// `E[(tr H)^2] = 8c` — and the fold is `b` times the root of that, which is the whole of
        /// what the curve is a function of. Drawn as two independent parts plus one shared: the
        /// shared draw is what makes `Hxx` and `Hyy` agree by `c`.
        ///
        /// Two hundred thousand draws a fold, which puts the standard error of each mean under
        /// 0.002 — a tenth of what is allowed, so a failure here is the fit and not the draw.
        TEST(RtxCausticGainTest, theFittedGainIsTheMeanOfWhatTheCausticComputes)
        {
            constexpr std::size_t draws = 200000;
            constexpr float shared = 1.0f / 8.0f;
            constexpr float own = 3.0f / 8.0f - shared;

            std::mt19937 gen(11);
            std::normal_distribution<float> normal(0.0f, 1.0f);

            // One field, every fold measured on it, so the folds share their draws and the curve
            // comes out smooth rather than eight independent estimates of eight points.
            std::vector<std::array<float, 3>> hessians;
            hessians.reserve(draws);
            for (std::size_t draw = 0; draw < draws; ++draw)
            {
                const float together = std::sqrt(shared) * normal(gen);
                hessians.push_back({ std::sqrt(own) * normal(gen) + together, std::sqrt(own) * normal(gen) + together,
                    std::sqrt(shared) * normal(gen) });
            }

            for (const float fold : { 0.5f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 3.5f, 4.0f })
            {
                // `E[(tr H)^2]` is one for the draws above, so the fold is the bend outright.
                double total = 0.0;
                for (const std::array<float, 3>& h : hessians)
                {
                    const float determinant = (1.0f - fold * h[0]) * (1.0f - fold * h[1]) - fold * fold * h[2] * h[2];

                    total += 1.0 / double{ std::max(std::abs(determinant), 1.0f / Shaders::WATER_CAUSTIC_MAX) };
                }

                EXPECT_NEAR(Shaders::causticGain(fold), static_cast<float>(total / draws), 0.02f)
                    << "at a fold of " << fold;
            }

            // **The second order is exact rather than fitted**, which is what the numerator's
            // coefficient being the denominator's plus one buys: a reciprocal of `1 - u` with `u`
            // of variance `f^2` is worth `1 + f^2` to second order, and the curve has to start
            // there whatever the draws say further out.
            EXPECT_FLOAT_EQ(Shaders::causticGain(0.0f), 1.0f) << "a flat sea gathers nothing";
            EXPECT_NEAR(Shaders::causticGain(0.1f), 1.01f, 0.001f) << "and a nearly flat one is 1 + f^2";
        }

        /// The waves gather the sun into moving lines on the bed, and move light rather than make it.
        ///
        /// Caustics are ray density — the determinant of the Jacobian of the map from where light
        /// met the surface to where it landed — and what isolates that term from everything else in
        /// the frame is a **ratio**: the same bed, camera and sun, rendered with a sea state and
        /// without one. Dividing the two cancels the albedo, the absorption, the cosine and the
        /// geometry, and leaves the term itself, pixel for pixel.
        ///
        /// **Looked at from underneath**, so no wavy surface stands between the eye and the bed.
        /// Seen from above, what the waves do to the *view* would be mixed into what they do to the
        /// *light*, and the ratio would measure both.
        TEST_F(RtxVisibilityTest, theWavesGatherSunlightOntoTheBedWithoutMakingAnyOfIt)
        {
            constexpr std::uint32_t size = 256;
            constexpr std::size_t count = std::size_t{ size } * size;
            constexpr float above = 540.0f;

            // A wide look from a fixed height over the bed *whatever the depth is*, so that two
            // depths see the same patch of water and their caustics can be compared pixel for pixel.
            //
            // **Wide enough to be an ensemble, and the pixel held where it was.** At ninety degrees
            // this covers 1080 units of bed — thirty correlation lengths across of the waves that
            // carry the curvature, and a thousand patches in the frame. A quarter of the width saw
            // sixty of them, and every mean it reported carried about ten per cent of sampling
            // error, which is five times what the tolerances below allow. The height and the pixel
            // count move together so that the footprint stays 4.2 units and the level the caustic
            // reads does not move with the fix.
            const auto render = [&](float depth, const SeaState& sea, float seconds) {
                const SceneDesc scene = makeFlooded(4000.0f, depth);

                Shaders::VisibilityConstants camera = makeCamera(osg::Vec3f(0.0f, -1.0f, above - depth),
                    osg::Vec3f(0.0f, 0.0f, -depth), 90.0f, size, size, 100000.0f);
                litThroughWater(camera);
                camera.mTime = seconds;

                std::vector<std::uint8_t> image;
                countHits(scene, {}, camera, size, image, { .mSea = sea });

                // **The radiance and not the byte, which is what a ratio of two dark pixels needs.**
                // Twenty metres of water leaves green at a fiftieth of the scale, where one step of
                // the display curve is a per cent of the ratio being measured — and every figure
                // here is a mean over that ratio.
                std::vector<float> linear;
                linear.reserve(count);
                for (std::size_t i = 0; i < count; ++i)
                    linear.push_back(mRadiance[i * 4 + 1]);

                return linear;
            };

            // Green, which at these depths has the most left to vary over: blue outlasts it but the
            // still sea is already bright in blue, and red is gone.
            const auto causticField = [&](float depth, float seconds = 0.0f) {
                // The still sea does not move, so one baseline serves whatever the clock says.
                const std::vector<float> still = render(depth, SeaState{ .mSignificantHeight = 0.0f }, 0.0f);
                const std::vector<float> running = render(depth, SeaState{}, seconds);

                std::vector<float> field;
                field.reserve(count);
                for (std::size_t i = 0; i < count; ++i)
                    field.push_back(running[i] / still[i]);

                return field;
            };

            // Two metres down, where a caustic is at its sharpest.
            const std::vector<float> shallow = causticField(140.0f);
            const auto [dimmest, brightest] = std::minmax_element(shallow.begin(), shallow.end());
            const float mean = meanOf(shallow);

            // Measured over this patch: the brightest place on the bed is gathered to 2.75 of what
            // a flat sea would put there and the dimmest thinned to 0.38, so the pattern is bold
            // rather than a wobble.
            EXPECT_GT(*brightest, 1.2f) << "measured 1.32, gathered into lines";
            EXPECT_LT(*dimmest, 0.8f) << "measured 0.72, and thinned between them";

            // **And the mean is one**, which is the claim that makes it light and not decoration.
            // A reciprocal of something that fluctuates is worth more than the reciprocal of its
            // mean, and `causticGain` is that excess divided back out. It comes out at 1.002 here.
            //
            // **The estimator it corrects reads 0.95 at two metres and 1.26 at twenty**, before the
            // fade blends either toward one — so what the gain takes out runs to a quarter of the
            // light where the slack allowed below is a fiftieth of it.
            EXPECT_NEAR(mean, 1.0f, 0.02f) << "the waves redistribute the sun, they do not make any";

            // **The pattern peaks in shallow water and fades as the inverse square root of the depth
            // past it**, which is Snyder and Dera's 1970 measurement of the sea and what every field
            // campaign since has found. Measured here 0.57, 0.53, 0.31, 0.062 and 0.027 at one, two,
            // six, twenty and forty metres.
            //
            // Two metres to six is the law almost exactly — 0.588 against the 0.589 it asks for.
            // Past that it falls faster, because `WATER_CAUSTIC_SPREAD` is broadening lines that
            // were coarse to begin with: the transform stops at half a metre of wavelength where a
            // real sea does not, so there is less fine structure to survive the blur.
            const std::vector<float> deeper = causticField(400.0f);
            const std::vector<float> deepest = causticField(1400.0f);

            EXPECT_LT(contrastOf(deeper), 0.7f * contrastOf(shallow)) << "six metres down, against two";
            EXPECT_LT(contrastOf(deepest), 0.35f * contrastOf(deeper)) << "and twenty, against six";

            // **And the mean holds at every one of them**, which is the half a single coefficient
            // could never do: the fold saturates at a metre of depth and the *read level* keeps
            // coarsening past it, so the estimator's own excess runs from 13 per cent at one metre
            // to 32 at twenty. One curve in the resolved fold follows all of it. The fade moves
            // nothing either way, because it blends toward one rather than scaling.
            EXPECT_NEAR(meanOf(deeper), 1.0f, 0.02f) << "measured 1.004, six metres down";
            EXPECT_NEAR(meanOf(deepest), 1.0f, 0.02f) << "and 0.998 at twenty";

            // **How bold the pattern is, and how fast it moves** — M6 asks for both measured rather
            // than eyeballed, and they are the two halves of one choice. The spectrum's short cutoff
            // is a limit in *time*, not in space: the waves that focus hardest are the shortest, and
            // a wave's period falls with its length, so the same waves that make the boldest
            // caustics are the ones that make them tear.
            // **A fifth less bold than the sinusoid table drew, and bought on purpose.** Curvature
            // weights a component by `A k²`, so a table of sixty-four had the shortest few owning
            // the Hessian outright — a handful of plane waves crossing, which focuses into hard
            // repeating lines and reads as a lattice. Tens of thousands of components at the same
            // wavelengths interfere into a mottle instead: the same energy, spread over every
            // direction rather than four, and no line drawn twice. It measures 0.223 against the
            // table's 0.277.
            EXPECT_NEAR(contrastOf(shallow), 0.213f, 0.02f) << "the pattern's contrast, as a fraction of its own mean";

            // A twelfth of a second, which is how long a frame is worth caring about. For two
            // samples of one field, `E[(b - a)^2] = 2 sigma^2 (1 - rho)`, so half the ratio of the
            // two sums is the share of the pattern that is new.
            //
            // **Taken against the best shift, because a pattern that travels is not one that
            // tears.** The sea carries its caustics shoreward at the waves' own phase speed, and a
            // field translated by a feature width scores near one on the unshifted sum while looking
            // perfectly coherent — so the raw figure counts the sea's own motion as decay. The least
            // over a small sweep of offsets separates them: the pattern travels a pixel here, and
            // that pixel is worth fourteen points of the 67 per cent the raw sum reports.
            //
            // **The sweep that put tearing at half was taken on a different surface.** 18 units of
            // wavelength reshuffled 73% and read as stripes running across the bottom, 32 came out
            // at 51%, 50 was dull at 33% — all of it over a table of sixty-four sinusoids, where the
            // shortest few owned the Hessian in four directions and the pattern was a lattice. A
            // lattice reshuffling is what reads as stripes. Tens of thousands of wavevectors
            // interfere into a mottle instead, and two shots a twelfth of a second apart show a net
            // that slides rather than one that boils.
            const std::vector<float> later = causticField(140.0f, 1.0f / 12.0f);

            float spread = 0.0f;
            for (std::size_t i = 0; i < count; ++i)
                spread += (shallow[i] - mean) * (shallow[i] - mean);

            // Three pixels either way, which is three times what the sea carries the pattern in a
            // twelfth of a second. The margin also keeps every shifted read inside the frame rather
            // than wrapping, since the two fields are the same patch of bed.
            constexpr int sweep = 3;
            constexpr int inside = int(size) - 2 * sweep;
            const auto newAfter = [&](int across, int down) {
                float apart = 0.0f;
                for (int y = sweep; y < int(size) - sweep; ++y)
                    for (int x = sweep; x < int(size) - sweep; ++x)
                    {
                        const float gap
                            = later[std::size_t(y + down) * size + (x + across)] - shallow[std::size_t(y) * size + x];
                        apart += gap * gap;
                    }

                return 0.5f * apart / spread * float(count) / (float(inside) * float(inside));
            };

            const float unshifted = newAfter(0, 0);
            float least = unshifted;
            for (int down = -sweep; down <= sweep; ++down)
                for (int across = -sweep; across <= sweep; ++across)
                    least = std::min(least, newAfter(across, down));

            EXPECT_NEAR(unshifted, 0.671f, 0.03f) << "how much of the pattern is new a twelfth later";
            EXPECT_NEAR(least, 0.529f, 0.03f) << "and how much of that the sea did not simply carry";
        }

        /// The sun's disc carries exactly its irradiance, however wide the pixel that finds it.
        ///
        /// **The sun is drawn in the sky, not answered by a highlight on each surface that could
        /// reflect it** — so the one thing that must hold is that widening the disc never changes
        /// how much light is in it. A cone twice as wide covers four times the solid angle and has
        /// to be four times dimmer, and that is what makes a rough sea spread the sun without
        /// brightening the sea.
        ///
        /// Two fields of view over the same sun, so the pixel doing the finding is ten times wider
        /// in one than the other. The irradiance is small because nothing here has an exposure stage
        /// and the real thing saturates on sight.
        TEST_F(RtxVisibilityTest, theSunsDiscCarriesItsIrradianceHoweverWideThePixelThatFindsIt)
        {
            constexpr std::uint32_t size = 64;
            constexpr float irradiance = 4.0e-5f;

            struct Disc
            {
                float mPeak;
                float mTotal;
                float mSpreadAngle;
            };

            // Looking straight at a sun 45 degrees up, from clear of the only thing in the scene.
            const auto lookAtTheSun = [&](float fov) {
                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -500.0f, 0.0f), osg::Vec3f(0.0f, -501.0f, 1.0f), fov, size, size, 10000.0f);
                camera.mSunPosition = sunStandingAt(osg::DegreesToRadians(45.0f));
                camera.mSunIrradiance = osg::Vec3f(irradiance, irradiance, irradiance);

                // **The disc is drawn because there is light, which is one fact and not two.** What
                // it is painted with is still its own colour, and white is the plain noon of it.
                camera.mSunDiscColour = osg::Vec3f(1.0f, 1.0f, 1.0f);

                const SceneDesc scene = makeWall();
                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels);

                // A pixel's solid angle is its side squared at these angles: the frame is two
                // degrees across in one case and twenty in the other, where the cos-cubed the exact
                // form carries is a part in two thousand.
                Disc disc{ .mPeak = 0.0f, .mTotal = 0.0f, .mSpreadAngle = camera.mCamera.mSpreadAngle };
                for (std::size_t i = 0; i < std::size_t{ size } * size; ++i)
                {
                    const float radiance = decodeSrgb(pixels[i * 4]);
                    disc.mPeak = std::max(disc.mPeak, radiance);
                    disc.mTotal += radiance * camera.mCamera.mSpreadAngle * camera.mCamera.mSpreadAngle;
                }
                return disc;
            };

            // The same arithmetic the shader does, from the same two constants: a cap of angular
            // radius `r` subtends `pi * (2 sin(r / 2))^2`, and the disc's radiance is the sun's
            // irradiance spread over it.
            const auto capRadiance = [&](float spreadAngle) {
                const float edge = 2.0f * std::sin(0.5f * (Shaders::SUN_ANGULAR_RADIUS + 0.5f * spreadAngle));
                return irradiance / (0.5f * Shaders::TAU * edge * edge);
            };

            // A byte is worth about 0.006 of a linear value up here, so that is the tolerance.
            const Disc fine = lookAtTheSun(2.0f);
            EXPECT_NEAR(fine.mPeak, capRadiance(fine.mSpreadAngle), 0.007f)
                << "a pixel a third of the sun's width across";

            // **And the light in it is the sun's**, integrated over the frame rather than argued
            // for. Two hundred and fifty-six pixels land inside this disc, so the sum is a real
            // quadrature: it comes out at 4.015e-5 against the 4.0e-5 that was put in.
            EXPECT_NEAR(fine.mTotal, irradiance, 0.03f * irradiance) << "the disc holds what the sun sent";

            // Ten times the field of view, so ten times the pixel and a disc widened by 1.6 — which
            // by the cap's solid angle is 2.28 times dimmer, and measures so. **This is the whole
            // mechanism**: the same flux over a larger cap.
            const Disc coarse = lookAtTheSun(20.0f);
            EXPECT_NEAR(coarse.mPeak, capRadiance(coarse.mSpreadAngle), 0.007f)
                << "a pixel wider than the sun, which averages it rather than sampling it";

            // Its total is not asserted, and the reason is the honest one: this disc covers 5.7
            // pixels and four pixel centres fall inside it, so a sum over pixels is quadrature with
            // four samples and lands 30% low. The peak above is the exact statement.
            EXPECT_LT(coarse.mPeak, fine.mPeak) << "a wider cone is a dimmer sun, never a brighter one";
        }

        /// A rough sea spreads the sun into a road across the water, and a flat one does not.
        ///
        /// **This is what the lost slopes are for.** Water too fine for the ray cone is averaged
        /// into a flat facet, and a facet that lost its slope is a mirror: it reflects the sun as
        /// one hard dot, and as the camera moves that dot jumps between pixels — the field of
        /// crawling white sparks that distant water becomes. Handing the variance of what was
        /// dropped to the sun's disc instead is what turns it into a road.
        ///
        /// Far enough out that the cone cannot resolve the waves, which is where the term decides
        /// the picture. Water and sky only, so every byte in the frame is the sun off the surface:
        /// the refraction ray leaves through the bottom, finds nothing, and comes back black.
        TEST_F(RtxVisibilityTest, aRoughSeaSpreadsTheSunIntoARoadAndAFlatOneShowsOneDot)
        {
            constexpr std::uint32_t size = 64;
            constexpr float irradiance = 1.0e-3f;

            struct Road
            {
                int mPeak;
                std::size_t mLit;
            };

            const auto road = [&](const SeaState& sea) {
                const osg::Vec3f eye(0.0f, 0.0f, 6000.0f);
                const osg::Vec3f at(0.0f, 14000.0f, 0.0f);

                Shaders::VisibilityConstants camera = makeCamera(eye, at, 20.0f, size, size, 1000000.0f);

                // The sun put exactly where the centre pixel's reflection points, which is the view
                // mirrored in the water's plane and then reversed into a direction of travel. Off
                // the top of the frame by twice the camera's own tilt, so the disc in the sky and
                // the disc in the water are never in the same picture.
                osg::Vec3f view = at - eye;
                view.normalize();
                camera.mSunPosition = osg::Vec3f(view.x(), view.y(), -view.z());
                camera.mSunIrradiance = osg::Vec3f(irradiance, irradiance, irradiance);
                camera.mSunDiscColour = osg::Vec3f(1.0f, 1.0f, 1.0f);
                camera.mWaterLevel = 0.0f;

                const SceneDesc scene = makeOpenWater(20000.0f);
                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels, { .mSea = sea });

                Road found{ .mPeak = 0, .mLit = 0 };
                for (std::size_t i = 0; i < std::size_t{ size } * size; ++i)
                {
                    found.mPeak = std::max(found.mPeak, int{ pixels[i * 4] });
                    found.mLit += pixels[i * 4] > 0 ? 1 : 0;
                }
                return found;
            };

            // A flat sea is a mirror and shows the sun once: four pixels of 4,096, at 202 of 255.
            const Road still = road(SeaState{ .mSignificantHeight = 0.0f });
            EXPECT_LT(still.mLit, 20u) << "measured 4 pixels: a mirror shows one dot";
            EXPECT_GT(still.mPeak, 150) << "measured 202: and shows it at full strength";

            // The same sun over a sea with a state in it reaches 1,914 pixels — near half the frame
            // — at a peak of 10. **Nearly five hundred times the area at a twentieth the strength**,
            // which is a road rather than a spark.
            const Road running = road(SeaState{});
            EXPECT_GT(running.mLit, 1000u) << "measured 1914 pixels: the sun spread across the water";
            EXPECT_LT(running.mPeak, 40) << "measured 10: and no pixel of it near the mirror's";
        }

        /// The sea runs the way the wind blows, and turning the wind turns the whole sea with it.
        ///
        /// **A rotation, checked as one.** The tiles are spread about their own +X and the frame
        /// turns them by `mSeaHeading` where they are read, so a sea under a north wind at world
        /// `p` is the sea under an east wind at `p` turned back — `(p.y, -p.x)`. Looked at straight
        /// down through a square orthographic frame centred on the origin, with right along +X and
        /// up along +Y, that is the picture turned a quarter turn: pixel `(i, j)` of the north frame
        /// is pixel `(N - 1 - j, i)` of the east one. A quarter turn maps the tiles' texels onto
        /// themselves, so the two frames sample the same texels and agree to the float.
        ///
        /// Lit so that a turn of the surface is a turn of the picture and nothing else: a sun at
        /// the zenith, an even sky, and open water with no bed for a turned refraction to land
        /// somewhere different on. The water's own march is jittered, so eight frames are averaged
        /// and the two are compared within a few bytes.
        TEST_F(RtxVisibilityTest, theSeaRunsTheWayTheWindBlows)
        {
            constexpr std::uint32_t size = 64;
            constexpr float across = 2000.0f;

            const auto looking = [&](const osg::Vec2f& heading, std::vector<std::uint8_t>& pixels) {
                const osg::Matrixf view = osg::Matrixf::lookAt(
                    osg::Vec3f(0.0f, 0.0f, 500.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), osg::Vec3f(0.0f, 1.0f, 0.0f));
                Shaders::VisibilityConstants camera
                    = makeOrthographicCameraFromView(view, across, across, size, size, 1.0f, 100000.0f);

                camera.mSunPosition = osg::Vec3f(0.0f, 0.0f, 1.0f);
                camera.mSunIrradiance = osg::Vec3f(4.0f, 4.0f, 4.0f);
                camera.mSunDiscColour = osg::Vec3f(1.0f, 1.0f, 1.0f);
                camera.mSkyHorizon = osg::Vec3f(0.5f, 0.5f, 0.5f);
                camera.mSkyZenith = camera.mSkyHorizon;
                camera.mWaterLevel = 0.0f;
                camera.mTime = 3.0f;
                camera.mSeaHeading = heading;

                const SceneDesc scene = makeOpenWater(20000.0f);
                countHits(scene, {}, camera, size, pixels, { .mFrames = 8 });
            };

            std::vector<std::uint8_t> east;
            std::vector<std::uint8_t> north;
            looking(osg::Vec2f(1.0f, 0.0f), east);
            looking(osg::Vec2f(0.0f, 1.0f), north);

            // The two are pictures of a running sea and not of a mirror, or a turn would show nothing.
            std::size_t varied = 0;
            for (std::size_t i = 4; i < east.size(); i += 4)
                varied += east[i] != east[i - 4] ? 1 : 0;
            ASSERT_GT(varied, std::size_t{ size } * size / 10) << "the sea is flat";

            std::size_t agreeing = 0;
            std::size_t turnedAway = 0;
            for (std::uint32_t j = 0; j < size; ++j)
                for (std::uint32_t i = 0; i < size; ++i)
                {
                    const std::size_t here = (std::size_t{ j } * size + i) * 4;
                    const std::size_t there = (std::size_t{ i } * size + (size - 1 - j)) * 4;
                    agreeing += std::abs(int{ north[here] } - int{ east[there] }) <= 3 ? 1 : 0;
                    turnedAway += std::abs(int{ north[here] } - int{ east[here] }) <= 3 ? 1 : 0;
                }

            EXPECT_GT(agreeing, std::size_t{ size } * size * 99 / 100) << "the north sea is the east sea turned";
            EXPECT_LT(turnedAway, std::size_t{ size } * size * 9 / 10) << "and not the east sea as it was";
        }

        /// Rain rings the still water under the eye, moves the rings on, and far off roughens it.

        ///
        /// **Close, the rings are read against a graded sky.** A resolved ring is a mirror facet,
        /// and a facet catches a quarter-degree sun only where its slope matches exactly — so a sun
        /// shows a handful of glints and says nothing about the rest. A sky that darkens toward the
        /// horizon shows every tilt: a facet leaning toward the horizon reflects a darker sky than
        /// the plane beside it. Forty units over a flat sea with a pixel about half a unit wide, a
        /// ring eleven centimetres across is sixteen pixels, and rain differs from a dry frame over
        /// a good share of it; a fifth of a second on — a third of a ring's life — it differs from
        /// itself. The dry frame does not care what time it is.
        ///
        /// **Far, the rings are read against the sun**, as the road test reads a running sea: from
        /// its height a pixel is hundreds of units and no ring is resolved, so what a ring lost is
        /// roughness, and the one dot a flat sea shows spreads into a road.
        ///
        /// A snowy frame is the plumbing's business and not the shader's: `mRainOnWater` is nought
        /// for snow before the frame is built, and `Weather::Precipitation` is where that is tested.
        TEST_F(RtxVisibilityTest, rainRingsTheWaterUnderTheEyeAndRoughensItFarOff)
        {
            constexpr std::uint32_t size = 64;
            constexpr std::size_t pixelCount = std::size_t{ size } * size;
            constexpr float irradiance = 1.0e-3f;

            // A flat sea, the eye at `eye` looking at `at`, and either a graded sky with no sun or
            // the road test's sun at the centre pixel's mirror under a black sky.
            const auto render = [&](const osg::Vec3f& eye, const osg::Vec3f& at, bool sunlit, float rain, float time,
                                    std::vector<std::uint8_t>& pixels) {
                Shaders::VisibilityConstants camera = makeCamera(eye, at, 20.0f, size, size, 1000000.0f);

                if (sunlit)
                {
                    osg::Vec3f view = at - eye;
                    view.normalize();
                    camera.mSunPosition = osg::Vec3f(view.x(), view.y(), -view.z());
                    camera.mSunIrradiance = osg::Vec3f(irradiance, irradiance, irradiance);
                    camera.mSunDiscColour = osg::Vec3f(1.0f, 1.0f, 1.0f);
                    camera.mSkyHorizon = osg::Vec3f();
                    camera.mSkyZenith = osg::Vec3f();
                }
                else
                {
                    camera.mSunIrradiance = osg::Vec3f();
                    camera.mSkyHorizon = osg::Vec3f(0.1f, 0.1f, 0.1f);
                    camera.mSkyZenith = osg::Vec3f(0.9f, 0.9f, 0.9f);
                }

                camera.mWaterLevel = 0.0f;
                camera.mRainOnWater = rain;
                camera.mTime = time;

                const SceneDesc scene = makeOpenWater(20000.0f);
                countHits(scene, {}, camera, size, pixels, { .mSea = SeaState{ .mSignificantHeight = 0.0f } });
            };

            const auto differing = [](const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
                std::size_t count = 0;
                for (std::size_t i = 0; i < a.size(); i += 4)
                    count += std::abs(int{ a[i] } - int{ b[i] }) > 2 ? 1 : 0;
                return count;
            };

            const osg::Vec3f near(0.0f, 0.0f, 40.0f);
            const osg::Vec3f nearAt(0.0f, 100.0f, 0.0f);

            std::vector<std::uint8_t> dry;
            std::vector<std::uint8_t> dryLater;
            std::vector<std::uint8_t> raining;
            std::vector<std::uint8_t> rainingLater;
            render(near, nearAt, false, 0.0f, 0.5f, dry);
            render(near, nearAt, false, 0.0f, 0.7f, dryLater);
            render(near, nearAt, false, 1.0f, 0.5f, raining);
            render(near, nearAt, false, 1.0f, 0.7f, rainingLater);

            EXPECT_EQ(dry, dryLater) << "a still sea does not care what time it is";
            EXPECT_GT(differing(raining, dry), pixelCount / 20) << "the rings tilt the sky's reflection";
            EXPECT_GT(differing(rainingLater, raining), pixelCount / 20) << "a fifth of a second on, elsewhere";

            const osg::Vec3f far(0.0f, 0.0f, 6000.0f);
            const osg::Vec3f farAt(0.0f, 14000.0f, 0.0f);

            const auto lit = [](const std::vector<std::uint8_t>& pixels) {
                std::size_t count = 0;
                for (std::size_t i = 0; i < pixels.size(); i += 4)
                    count += pixels[i] > 0 ? 1 : 0;
                return count;
            };

            std::vector<std::uint8_t> farDry;
            std::vector<std::uint8_t> farRain;
            render(far, farAt, true, 0.0f, 0.5f, farDry);
            render(far, farAt, true, 1.0f, 0.5f, farRain);
            EXPECT_LT(lit(farDry), 20u) << "a still sea shows the sun once";
            EXPECT_GT(lit(farRain), 200u) << "what the cone could not resolve widened the sun's lobe";
        }

        /// Water too fine for the cone to resolve widens the cone that refracts through it.
        ///
        /// **What the cone could not resolve is not gone, it is rough** — and that roughness has to
        /// reach the texture filter, not only the specular lobe. A seabed seen through a mile of
        /// ruffled water is blurred by the slopes that were averaged away, and read at its sharpest
        /// mip instead it comes back as crawling detail no filter downstream can take out.
        ///
        /// **The sea here is built so that every wave is past the cone's reach.** The spectrum runs
        /// from 32 units up to its peak, so with a peak of 64 a footprint of 66 resolves none of it:
        /// every slope is averaged out and the surface is geometrically flat — the normal is exactly
        /// up, the refraction goes straight down, and nothing wobbles — while the whole spectrum's
        /// variance is still carried as roughness. That leaves the cone's width as the only thing
        /// that differs from a still sea, and a ladder of mips on the bed to read it off.
        TEST_F(RtxVisibilityTest, waterTooFineToResolveWidensTheConeItRefractsThrough)
        {
            constexpr std::uint32_t size = 64;
            constexpr float height = 12000.0f;
            constexpr float depth = 1500.0f;

            // How far either way from the middle the cone is read. The bed is a thousand units
            // across and a pixel covers 74 of them, so the sheet is thirteen pixels wide and this
            // leaves a pixel of margin at each edge — no cone in the patch straddles the rim.
            constexpr std::uint32_t across = 5;

            TestTexture ladder;
            makeMipLadder(ladder);
            const std::span<const TextureData> textures(&ladder.mData, 1);

            // A fifth of a degree off the vertical, which is the least `makeCamera` will take.
            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -50.0f, height), osg::Vec3f(0.0f, 0.0f, -depth), 20.0f, size, size, 100000.0f);
            camera.mWaterLevel = 0.0f;

            // Water over a bed that glows its own ladder. Emissive rather than lit, so the pixel is
            // the texture and nothing else: an unlit albedo would need a light, and a light would
            // put its own falloff between the mip and the measurement.
            //
            // **A patch of the bed and not one pixel of it, which is the whole of what this used to
            // get wrong.** `mLostSlope` is a mean over the cone's own footprint — 66 units against a
            // spectrum that stops at 32, so about four correlation cells of the waves that carry the
            // slope. One reading of that is not an ensemble mean: measured over this patch the level
            // has a standard deviation of 0.12, and the centre pixel stood 0.8 of one away from the
            // patch's own mean.
            //
            // **The radiance and not the byte.** One byte of the display curve is 0.08 of a mip
            // level here, which is four times the tolerance below.
            //
            // The mean of the cones rather than of their levels, because a level is a logarithm and
            // what the prediction below names is a width.
            const auto coneOver = [&](const SeaState& sea) {
                SceneDesc scene = makeOpenWater(4000.0f);

                const Index bed = scene.addMesh(makeSheet(500.0f, -depth), {}, sQuadUv, sQuadIndices);
                const Index glow = scene.addMaterial(
                    Material{ .mEmissive = scene.addTexture(VFS::Path::NormalizedView("ladder.dds")) });
                scene.addInstance(
                    MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = bed, .mMaterial = glow });

                std::vector<std::uint8_t> pixels;
                countHits(scene, textures, camera, size, pixels, { .mSea = sea });

                // Back out everything between the texture and the radiance: the emissive scale, the
                // water the glow crossed on its way up, and the two per cent the surface reflected.
                const float carried = Shaders::EMISSIVE_INTENSITY * std::exp(-Shaders::WATER_EXTINCTION[1] * depth)
                    * (1.0f - Shaders::WATER_F0);

                float total = 0.0f;
                for (std::uint32_t y = size / 2 - across; y <= size / 2 + across; ++y)
                    for (std::uint32_t x = size / 2 - across; x <= size / 2 + across; ++x)
                        total += std::exp2(ladderLevel(mRadiance[(std::size_t{ y } * size + x) * 4 + 1] / carried));

                // In ladder texels, which is a width up to the one scale that cancels below.
                return total / static_cast<float>((2 * across + 1) * (2 * across + 1));
            };

            // How wide the refraction's cone is where it lands, in world units: the pixel's own
            // footprint where it met the water, plus what it gained over the leg down. **Twice the
            // lobe**, because the lobe is an rms angle from the axis and a cone spread is a width —
            // and a quarter of it to begin with, because refraction bends by `1 - 1 / n` of what
            // reflection does, so what is seen *through* a rough surface is blurred that much less.
            // The pixel's cone where it met the water, which is both what the ladder is read through
            // and what `waveLevel` picks a mip by — one quantity, so one name.
            const float footprint = camera.mCamera.mSpreadAngle * height;

            const auto coneAtBed = [&](float lobe) {
                const float bent = lobe * (1.0f - 1.0f / Shaders::WATER_IOR);
                return footprint + (camera.mCamera.mSpreadAngle + 2.0f * bent) * depth;
            };

            const SeaState fine{ .mSignificantHeight = 3.0f, .mPeakWavelength = 64.0f };
            const float still = coneOver(SeaState{ .mSignificantHeight = 0.0f });
            const float ruffled = coneOver(fine);

            // The ladder has seven levels and the readout is only meaningful off both ends of it.
            EXPECT_GT(std::log2(still), 1.0f) << "measured 2.25, clear of the sharpest mip";
            EXPECT_LT(std::log2(ruffled), 5.0f) << "measured 3.68, clear of the coarsest";

            // Mip level is the log of the cone's width, so the ratio of the two widths is the ratio
            // of the cones — and the base term, which is the triangle's texels against its world
            // area, cancels.
            //
            // **The lobe is asked of the chain and not of the spectrum.** `waveLevel` reads at
            // `log2(footprint / texel)`, so the slope the shader carries is what a mip that wide
            // averaged away and never the whole of it — and `RtxWavePassTest` is what says the chain
            // and `lostSlopeOf` agree about that to within the half floats it is stored in.
            //
            // **The tolerance is a fifth of what it was, and what is left is the two Jensen terms.** The
            // patch's mean cone is a mean over pixels of a term in `sqrt(lost)`, where the
            // prediction takes the root of the mean — 0.9 per cent low at this spread — and the log
            // of a mean stands 0.005 of a level over the mean of the logs. Both are computed rather
            // than allowed for, and together they are under a hundredth. Measured 1.433 against a
            // prediction of 1.425.
            //
            // What the assertion settles is the optics: the factor of two, because a normal tilted
            // by an angle turns a reflection by twice it, and the `1 - 1/n` that says a refraction
            // is bent by a quarter of what a reflection is. Adding the lobe once instead of twice
            // puts the prediction at 0.882, twenty-seven tolerances away.
            EXPECT_NEAR(
                std::log2(ruffled / still), std::log2(coneAtBed(lobeOf(fine, footprint)) / coneAtBed(0.0f)), 0.02f)
                << "the cone widened by twice the rms slope the sea could not show";
        }
    }
}
