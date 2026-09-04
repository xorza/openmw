#include "fixture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace Rtx::Testing
{
    namespace
    {
        /// The denoiser, measured against the estimator it is smoothing.
        ///
        /// One flat floor under an open sky, so every pixel is one bounce off the same normal with
        /// the same answer in expectation: the mean is fixed and the scatter around it is pure
        /// sampling noise. A filter has one job on a surface like this — take the scatter away and
        /// leave the mean where it was — and both halves are asserted, because a filter that dimmed
        /// the picture would pass a test that only looked at the noise.
        TEST_F(RtxVisibilityTest, theFilterTakesTheNoiseOffAFlatSurfaceAndLeavesTheLightWhereItWas)
        {
            constexpr std::uint32_t size = 64;
            constexpr float samples = float{ size } * size;

            SceneDesc scene;
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                .mMesh = scene.addMesh(makeSheet(4000.0f, 0.0f), {}, {}, sQuadIndices) });

            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -1.0f, 300.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
            camera.mSkyHorizon = osg::Vec3f(0.20f, 0.15f, 0.60f);
            camera.mSkyZenith = osg::Vec3f(0.80f, 0.65f, 0.15f);
            camera.mAmbientFromSky = 1.0f;

            const auto shade = [&](bool filter) {
                std::vector<std::uint8_t> pixels;
                EXPECT_EQ(countHits(scene, {}, camera, size, pixels, { .mFilter = filter }), size * size);
                return pixels;
            };

            const auto measure = [&](const std::vector<std::uint8_t>& pixels, std::size_t channel) {
                float sum = 0.0f;
                float squares = 0.0f;
                for (std::size_t i = channel; i < pixels.size(); i += 4)
                {
                    const float linear = decodeSrgb(pixels[i]);
                    sum += linear;
                    squares += linear * linear;
                }

                const float mean = sum / samples;
                return std::pair{ mean, std::sqrt(std::max(squares / samples - mean * mean, 0.0f)) };
            };

            const std::vector<std::uint8_t> raw = shade(false);
            const std::vector<std::uint8_t> filtered = shade(true);

            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                const auto [rawMean, rawSpread] = measure(raw, channel);
                const auto [filteredMean, filteredSpread] = measure(filtered, channel);

                // Half an sRGB step at this brightness, which is the most the two can differ by
                // without one of them having moved the light.
                EXPECT_NEAR(filteredMean, rawMean, 0.004f) << "channel " << channel << " keeps its light";

                EXPECT_LT(filteredSpread, rawSpread * 0.2f)
                    << "channel " << channel << " has most of its noise taken away";
            }
        }

        /// The same floor at a grazing angle, against the answer it is trying to reach.
        ///
        /// **Terrain is nearly always seen this way, and it is the case a depth test gets wrong.**
        /// Pixels down a grazing surface stand a long way apart in distance while remaining one
        /// flat plane, so a filter that refused taps by how far away they are keeps only the taps
        /// across the slope and throws away the ones along it — it still smooths, just half as
        /// well, which is why this measures the error rather than the smoothness. Weighing by how
        /// far a tap sits off the centre pixel's tangent plane costs one dot product and asks the
        /// question that was meant.
        ///
        /// The reference is what `--accumulate` builds: sixty-four differently seeded samples of
        /// the same unbiased estimator, averaged. One sample against that is the error a denoiser
        /// exists to reduce, and the ratio of the two is the only honest way to say it worked.
        ///
        /// **The bound sits between the two weightings on purpose.** Measured here on
        /// `Channel::Radiance`: one sample is 0.0420 off the reference and the plane weight brings
        /// that to 0.0020, against 0.0061 for a plain depth weight — twenty times better against
        /// seven. Every number is repeatable, because frame zero and a sixty-four frame average are
        /// both deterministic, so a tenth is a bound this passes with room and a depth test cannot
        /// reach.
        TEST_F(RtxVisibilityTest, theFilterAndItsHistoryConvergeOnAGrazingSurface)
        {
            constexpr std::uint32_t size = 64;

            SceneDesc scene;
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                .mMesh = scene.addMesh(makeSheet(40000.0f, 0.0f), {}, {}, sQuadIndices) });

            // A degree and a half above the floor: the horizon sits near the top of the frame and
            // the ground runs from a few hundred units away to eight thousand, so the distance
            // between vertical neighbours changes by more than a pixel footprint nearly everywhere.
            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -8000.0f, 200.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
            camera.mSkyHorizon = osg::Vec3f(0.20f, 0.15f, 0.60f);
            camera.mSkyZenith = osg::Vec3f(0.80f, 0.65f, 0.15f);
            camera.mAmbientFromSky = 1.0f;

            const auto render = [&](std::uint32_t accumulate, bool filter) {
                std::vector<float> values;
                renderRadiance(scene, camera, size, values, { .mFrames = accumulate, .mFilter = filter });
                return values;
            };

            // Unfiltered, because a thousand filtered frames converge on the filter's opinion and
            // not on the answer.
            const std::vector<float> reference = render(64, false);
            const std::vector<float> raw = render(0, false);
            const std::vector<float> filtered = render(0, true);

            const auto errorAgainstReference = [&](const std::vector<float>& values) {
                float squares = 0.0f;
                std::size_t counted = 0;
                for (std::size_t i = 1; i < values.size(); i += 4)
                {
                    // Only where there is a surface: the sky above the horizon is not being
                    // filtered and averages to itself, so counting it would dilute both figures.
                    if (values[i] == reference[i] && values[i] == 0.0f)
                        continue;

                    const float error = values[i] - reference[i];
                    squares += error * error;
                    ++counted;
                }

                return std::sqrt(squares / static_cast<float>(counted));
            };

            const float before = errorAgainstReference(raw);
            const float after = errorAgainstReference(filtered);

            EXPECT_GT(before, 0.02f) << "one sample is noisy enough here for the question to mean something";
            EXPECT_LT(after, before * 0.10f)
                << "and the filter takes most of that error away: " << before << " becomes " << after;

            // **And the temporal half, which is where the error actually falls.** The spatial cascade
            // borrows samples sideways from neighbours looking at the same surface, which has a
            // floor: there are only so many of those and they are correlated. Averaging over frames
            // borrows from samples that are genuinely independent, so its error keeps falling for as
            // long as the history is allowed to grow.
            //
            // Sequenced frames with no averaging in the composite: the sampler advances, so each
            // frame is a different draw, and the only thing combining them is the accumulator under
            // test. A still camera, so every pixel reprojects onto itself and no history is
            // rejected — which is the case this has to get right before any other.
            //
            // **`resetHistory` before each run, and leaving it out is what made this test lie.** The
            // fixture renders many frames, the accumulator keeps what it built across all of them,
            // and a "one frame" baseline taken without a reset is a baseline that already has a
            // history in it — which reads as the accumulator doing nothing at all.
            const auto renderSequence = [&](std::uint32_t frames) {
                std::vector<float> values;
                renderFiltered(scene, camera, size, values, frames);
                return values;
            };

            const std::vector<float> settledPixels = renderSequence(Shaders::ACCUMULATE_FRAMES);
            const float settled = errorAgainstReference(settledPixels);

            // **The accumulator may not make this worse, and on this surface that is the whole of
            // what it can be asked.** Measured here, the cascade alone already lands at 0.0020 of
            // the converged reference — a flat sheet under a smooth sky is precisely
            // where five levels of à-trous have every advantage, since the signal is uniform and
            // every neighbour is a valid sample of it. What the history is for is the case this
            // scene does not have: contact regions, small geometry, and pixels with few neighbours
            // looking at the same thing, which is what
            // `theHistoryCarriesWhereTheCascadeHasNoNeighboursToBorrow` is for.
            //
            // **Eight per cent rather than five, because the history is the filtered light now.**
            // The cascade keeps its levels in half floats, which puts a rounding floor of about 3e-4
            // of the value under a figure the cascade had already driven to 0.0020 — so past that
            // point this is measuring a storage format and not an accumulator. Measured on this box:
            // at full width the pair is 0.00201 and 0.00203, at half width it was 0.00201 and
            // 0.00210, and with SVGF's feedback it is 0.00201 and 0.00213, against an unfiltered
            // 0.042. `.notes/rtx/shader-review.md` §4 says what the width was worth and what it cost.
            //
            // **A flat sheet is where feeding the filtered light back has least to give**, since the
            // cascade has every neighbour it could want and averaging its answers over frames only
            // correlates them. What the feedback is for is the other scene, and
            // `theHistoryCarriesWhereTheCascadeHasNoNeighboursToBorrow` moved by nothing there:
            // 0.00272 settled against 0.00475 alone, which is the ratio that test already records.
            EXPECT_LE(settled, after * 1.08f)
                << "the history does not cost what the cascade gained: " << after << " becomes " << settled;

            // **And it converges toward the reference rather than toward its own opinion.** An
            // average that drifted would still be quieter, and quieter is not the claim: the mean of
            // the settled picture has to sit where the converged one does, or the accumulator is
            // dimming the frame and calling it denoising.
            double settledMean = 0.0;
            double referenceMean = 0.0;
            std::size_t counted = 0;
            for (std::size_t i = 1; i < settledPixels.size(); i += 4)
            {
                if (settledPixels[i] == reference[i] && settledPixels[i] == 0.0f)
                    continue;

                settledMean += static_cast<double>(settledPixels[i]);
                referenceMean += static_cast<double>(reference[i]);
                ++counted;
            }

            ASSERT_GT(counted, 0u);
            settledMean /= static_cast<double>(counted);
            referenceMean /= static_cast<double>(counted);

            EXPECT_NEAR(settledMean, referenceMean, referenceMean * 0.02)
                << "the accumulated mean is " << settledMean << " against a converged " << referenceMean;
        }

        /// A floor meeting a wall, and the filter keeping them apart.
        ///
        /// **Everything else here would pass with a plain blur.** Smoothing noise and preserving a
        /// mean are what any average does; what makes this a denoiser rather than a soft-focus
        /// filter is that it refuses to mix two surfaces that happen to be neighbours on screen.
        ///
        /// So this measures the one place where that shows: the step from one row to the next
        /// across the crease. Away from it a blur is nearly harmless, because five levels of a
        /// B3 kernel put most of their weight near the centre however far the taps reach — which is
        /// exactly why a test comparing the two ends of the frame passes with the guide switched
        /// off, and this one does not.
        ///
        /// The crease is found rather than assumed: it is the row boundary where the unfiltered
        /// picture jumps hardest, which is where the geometry says it should be. The two surfaces
        /// are told apart by their normals alone, and lit differently for the same reason — a
        /// floor's cosine-weighted hemisphere is centred on the zenith and a wall's lies along the
        /// horizon, and this sky runs a long way between the two.
        TEST_F(RtxVisibilityTest, theFilterWillNotMixAFloorIntoTheWallStandingOnIt)
        {
            constexpr std::uint32_t size = 64;

            const std::array wall{
                osg::Vec3f(-2000.0f, 0.0f, 0.0f),
                osg::Vec3f(2000.0f, 0.0f, 0.0f),
                osg::Vec3f(2000.0f, 0.0f, 4000.0f),
                osg::Vec3f(-2000.0f, 0.0f, 4000.0f),
            };

            SceneDesc scene;
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                .mMesh = scene.addMesh(makeSheet(4000.0f, 0.0f), {}, {}, sQuadIndices) });
            scene.addInstance(MeshInstance{
                .mTransform = osg::Matrixf::identity(), .mMesh = scene.addMesh(wall, {}, {}, sQuadIndices) });

            // The floor fills the bottom of the frame and the wall the top, with the crease running
            // straight across the middle of it.
            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -1200.0f, 900.0f), osg::Vec3f(0.0f, 0.0f, 250.0f), 60.0f, size, size, 100000.0f);
            camera.mSkyHorizon = osg::Vec3f(0.20f, 0.15f, 0.60f);
            camera.mSkyZenith = osg::Vec3f(0.80f, 0.65f, 0.15f);
            camera.mAmbientFromSky = 1.0f;

            // Green, where this sky has its widest range between the horizon and the zenith. A row
            // at a time, so that sixty-four pixels stand behind every number and the sampling noise
            // that is left cannot be mistaken for a step.
            const auto rowMeans = [&](std::uint32_t accumulate, bool filter) {
                std::vector<std::uint8_t> pixels;
                EXPECT_EQ(countHits(scene, {}, camera, size, pixels, { .mFrames = accumulate, .mFilter = filter }),
                    size * size);

                std::array<float, size> rows{};
                for (std::uint32_t y = 0; y < size; ++y)
                {
                    float sum = 0.0f;
                    for (std::uint32_t x = 0; x < size; ++x)
                        sum += decodeSrgb(pixels[(std::size_t{ y } * size + x) * 4 + 1]);

                    rows[y] = sum / size;
                }

                return rows;
            };

            // Where the crease is, off a converged frame rather than a noisy one. A single sample's
            // row means swing by more than the step does, so asking a noisy picture where its
            // biggest jump is answers with the loudest pixel and not with the geometry.
            const std::array<float, size> converged = rowMeans(64, false);

            std::uint32_t crease = 0;
            for (std::uint32_t y = 1; y < size; ++y)
                if (std::abs(converged[y] - converged[y - 1]) > std::abs(converged[crease + 1] - converged[crease]))
                    crease = y - 1;

            const float truth = std::abs(converged[crease + 1] - converged[crease]);
            const std::array<float, size> filtered = rowMeans(0, true);
            const float kept = std::abs(filtered[crease + 1] - filtered[crease]);

            ASSERT_GT(truth, 0.02f) << "the two surfaces have to part company for this to mean anything";
            EXPECT_GT(kept, truth * 0.7f) << "the step at row " << crease << " survives the filter: it is " << truth
                                          << " in the converged frame and " << kept << " in the filtered one";
        }

        /// The exposure moves toward what it measured rather than snapping to it.
        ///
        /// **Adaptation is a time-domain thing, and this was the only term in the frame without
        /// one.** The histogram was measured on the frame the curve was about to map and applied to
        /// that same frame, so any one-frame excursion in it was a one-frame excursion in the whole
        /// image — and the degenerate branch could take a night exterior from an exposure of order
        /// tens to exactly one between two frames.
        ///
        /// Two skies a factor of thirty-two apart and nothing else in the picture, so the histogram
        /// is the only thing that changed. Both halves are claimed: told it has no past, the eye
        /// arrives at once; told it has one, it has barely moved a frame later.
        ///
        /// **Driven frame by frame rather than through `countHits`**, because that helper calls
        /// `setScene` every time and a new scene clears the previous camera — which is a reset, and
        /// a reset is exactly what the middle frame here must not have.
        TEST_F(RtxVisibilityTest, theExposureMovesTowardWhatItMeasuresRatherThanSnappingToIt)
        {
            constexpr std::uint32_t size = 32;

            Shaders::VisibilityConstants bright = makeCamera(
                osg::Vec3f(0.0f, 0.0f, 200.0f), osg::Vec3f(0.0f, 1000.0f, 200.0f), 60.0f, size, size, 100000.0f);
            bright.mSkyHorizon = osg::Vec3f(0.8f, 0.8f, 0.8f);
            bright.mSkyZenith = bright.mSkyHorizon;
            bright.mAmbientFromSky = 1.0f;

            Shaders::VisibilityConstants dim = bright;
            dim.mSkyHorizon = bright.mSkyHorizon / 32.0f;
            dim.mSkyZenith = dim.mSkyHorizon;
            dim.mAmbientFromSky = 1.0f;

            std::vector<std::uint8_t> pixels;

            const auto meanByte = [&pixels] {
                double sum = 0.0;
                std::size_t counted = 0;
                for (std::size_t i = 0; i < pixels.size(); i += 4)
                    for (std::size_t channel = 0; channel < 3; ++channel)
                    {
                        sum += pixels[i + channel];
                        ++counted;
                    }

                return counted > 0 ? sum / static_cast<double>(counted) : 0.0;
            };

            // The exposure measured rather than pinned, which is the whole subject.
            const auto shot = [&](const Shaders::VisibilityConstants& camera) {
                mRenderer->renderFrame(camera, FrameOptions{ .mExposure = std::nullopt });
                mRenderer->readPixels(pixels);
                return meanByte();
            };

            // Nothing to hit, so every pixel is the sky and the mean of the frame is the sky. The
            // scene is set once: setting it again would clear the previous camera and reset the eye.
            mRenderer->resize(size, size);
            mRenderer->setScene(Rtx::sWorld, SceneDesc{}, {}, SeaState{});

            const double lit = shot(bright);
            ASSERT_GT(lit, 0.0) << "the bright sky rendered as black";

            // The same sky thirty-two times darker, one frame later and with a past to move from.
            // The eye has had a few milliseconds against a time constant of a second and a half, so
            // it has gone almost nowhere — a stall of a third of a second would still leave it so.
            const double justAfter = shot(dim);

            // And the same sky again with no past, which is where it is headed.
            mRenderer->resetHistory();
            const double adapted = shot(dim);

            EXPECT_GT(adapted, 0.0) << "the dark sky rendered as black even with the eye open";
            EXPECT_LT(justAfter, 0.5 * adapted)
                << "the exposure arrived in one frame: " << justAfter << " against " << adapted;
        }

        /// A reset survives a frame that has no history to reset.
        ///
        /// **`resetHistory` is spent by the frame that answers it, and a frame with neither denoiser
        /// answers nothing.** `Denoiser::None` runs no accumulator and `Upscale::Off` runs no
        /// upscaler, so nothing reads the signal — and a renderer that cleared it at the end of every
        /// frame regardless dropped the reset rather than deferring it. What the game does with that
        /// is walk through a door on an unfiltered frame and reproject one room onto another on the
        /// next filtered one.
        ///
        /// **The claim is exact rather than statistical.** An accumulator that was reset writes the
        /// frame's own sample and reads no past at all, so the picture is the one the same trace
        /// makes from a fresh reset — value for value, since the frame index is what seeds the
        /// sampler and the camera never moves. The accumulated frame between them is what proves the
        /// comparison can tell a history from none.
        TEST_F(RtxVisibilityTest, aResetSurvivesAFrameThatHasNoHistoryToReset)
        {
            constexpr std::uint32_t size = 64;

            // The index every measured frame is drawn at, so the three of them differ only in what
            // the accumulator was handed. The frames around them take other indices.
            constexpr std::uint32_t measured = 3;

            SceneDesc scene;
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                .mMesh = scene.addMesh(makeSheet(40000.0f, 0.0f), {}, {}, sQuadIndices) });

            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -8000.0f, 200.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
            camera.mSkyHorizon = osg::Vec3f(0.20f, 0.15f, 0.60f);
            camera.mSkyZenith = osg::Vec3f(0.80f, 0.65f, 0.15f);
            camera.mAmbientFromSky = 1.0f;

            mRenderer->resize(size, size);
            mRenderer->setScene(Rtx::sWorld, scene, {}, SeaState{});

            const auto renderOne = [&](std::uint32_t frame, bool filter) {
                Shaders::VisibilityConstants sampled = camera;
                sampled.mFrame = frame;
                mRenderer->renderFrame(sampled, FrameOptions{ .mAccumulate = 0, .mFilter = filter, .mExposure = 1.0f });
            };

            const auto radiance = [&] {
                std::vector<float> values;
                mRenderer->readChannel(Channel::Radiance, values);
                return values;
            };

            const auto mostTheyDifferBy = [](const std::vector<float>& left, const std::vector<float>& right) {
                float most = 0.0f;
                for (std::size_t i = 0; i < left.size(); ++i)
                    most = std::max(most, std::abs(left[i] - right[i]));

                return most;
            };

            // A previous camera to reproject against, so nothing below is reset by the zero basis
            // that catches a renderer's first frame instead of by the flag under test.
            renderOne(measured + 1, true);

            mRenderer->resetHistory();
            renderOne(measured, true);
            const std::vector<float> single = radiance();

            // The same trace with a history behind it, at indices the sampler has not drawn yet so
            // the mean is over genuinely different draws.
            for (std::uint32_t frame = 0; frame < Shaders::ACCUMULATE_FRAMES; ++frame)
                renderOne(frame + 100, true);

            renderOne(measured, true);
            const std::vector<float> accumulated = radiance();

            ASSERT_EQ(accumulated.size(), single.size());
            ASSERT_GT(mostTheyDifferBy(accumulated, single), 0.0f)
                << "a history behind the same trace has to change the picture, or this proves nothing";

            // The frame under test sits between the reset and the frame that can act on it, and
            // reads the signal nowhere.
            mRenderer->resetHistory();
            renderOne(measured + 2, false);
            renderOne(measured, true);
            const std::vector<float> carried = radiance();

            ASSERT_EQ(carried.size(), single.size());
            EXPECT_EQ(mostTheyDifferBy(carried, single), 0.0f) << "the unfiltered frame spent a reset it could not use";
        }

        /// What the history is worth where the cascade has nothing to borrow from.
        ///
        /// **The grazing sheet above is the wavelet's best case and cannot answer this.** Every pixel
        /// of it looks at one flat surface under one smooth sky, so every pixel has the *same*
        /// expected bounce — a hundred and twenty-five taps average a hundred and twenty-five draws
        /// from one distribution, and the error falls by the square root of that. It converges to
        /// within a third of a byte on a single frame, and a history cannot improve on nothing left
        /// to remove.
        ///
        /// So this is the other case, and it is the one Morrowind's geometry actually is: a surface
        /// whose neighbours disagree. The sheet is cut into a grid of coplanar cells whose *shading*
        /// normals alternate by forty degrees, which is far outside what `mNormalPower` lets a tap
        /// carry — so the plane test passes everywhere, the normal test rejects nearly every
        /// neighbour, and the cascade is left with little more than the centre pixel. Nothing about
        /// the accumulator changes: a still camera reprojects every pixel onto itself.
        ///
        /// The alternating tilt is not a trick to defeat the filter. It is what a bumpy surface is,
        /// and the reason the two neighbours may not be averaged is that they are genuinely lit
        /// differently — a cosine lobe tilted forty degrees samples a different part of this sky.
        TEST_F(RtxVisibilityTest, theHistoryCarriesWhereTheCascadeHasNoNeighboursToBorrow)
        {
            constexpr std::uint32_t size = 64;
            constexpr int cells = 16;
            constexpr float extent = 4000.0f;
            constexpr float tilt = 20.0f;

            std::vector<osg::Vec3f> positions;
            std::vector<osg::Vec3f> normals;
            std::vector<std::uint32_t> indices;
            for (int y = 0; y < cells; ++y)
                for (int x = 0; x < cells; ++x)
                {
                    const float lowX = -extent + 2.0f * extent * static_cast<float>(x) / cells;
                    const float highX = -extent + 2.0f * extent * static_cast<float>(x + 1) / cells;
                    const float lowY = -extent + 2.0f * extent * static_cast<float>(y) / cells;
                    const float highY = -extent + 2.0f * extent * static_cast<float>(y + 1) / cells;

                    const auto base = static_cast<std::uint32_t>(positions.size());
                    positions.emplace_back(lowX, lowY, 0.0f);
                    positions.emplace_back(highX, lowY, 0.0f);
                    positions.emplace_back(highX, highY, 0.0f);
                    positions.emplace_back(lowX, highY, 0.0f);

                    // Coplanar, so nothing here is a step in the geometry; only the normal moves.
                    const float lean = osg::DegreesToRadians((x + y) % 2 == 0 ? tilt : -tilt);
                    const osg::Vec3f leaning(std::sin(lean), 0.0f, std::cos(lean));
                    for (int corner = 0; corner < 4; ++corner)
                        normals.push_back(leaning);

                    for (const std::uint32_t offset : sQuadIndices)
                        indices.push_back(base + offset);
                }

            SceneDesc scene;
            scene.addInstance(MeshInstance{
                .mTransform = osg::Matrixf::identity(), .mMesh = scene.addMesh(positions, normals, {}, indices) });

            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -2600.0f, 2600.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);

            // A sky that changes a great deal between the two tilts, so that two neighbouring cells
            // really do have different answers and averaging them really is wrong.
            camera.mSkyHorizon = osg::Vec3f(0.90f, 0.10f, 0.05f);
            camera.mSkyZenith = osg::Vec3f(0.05f, 0.20f, 0.90f);
            camera.mAmbientFromSky = 1.0f;

            const auto renderSequence = [&](std::uint32_t frames) {
                std::vector<float> values;
                renderFiltered(scene, camera, size, values, frames);
                return values;
            };

            // Unfiltered, because a converged reference has to be the answer and not the filter's
            // opinion of it.
            std::vector<float> reference;
            renderRadiance(scene, camera, size, reference, { .mFrames = 128 });

            const auto errorAgainstReference = [&](const std::vector<float>& values) {
                double squares = 0.0;
                std::size_t counted = 0;
                for (std::size_t i = 0; i < values.size(); i += 4)
                    for (std::size_t channel = 0; channel < 3; ++channel)
                    {
                        const std::size_t at = i + channel;
                        // Only where the grid is: the sky around it is not being filtered.
                        if (values[at] == reference[at] && reference[at] == 0.0f)
                            continue;

                        const double error = static_cast<double>(values[at]) - static_cast<double>(reference[at]);
                        squares += error * error;
                        ++counted;
                    }

                return std::sqrt(squares / static_cast<double>(counted));
            };

            const std::vector<float> settledPixels = renderSequence(Shaders::ACCUMULATE_FRAMES);
            const double settled = errorAgainstReference(settledPixels);

            // **The cascade alone over the same sixteen draws the history averaged, pooled as a root
            // mean square.** One frame's error swings by four per cent with the draw it got, so a
            // single frame under this ratio put it at the mercy of the sampler's stream: anything that
            // reshuffled the stream moved the figure by a fiftieth, and a bound a fiftieth above it
            // failed on a change that touched neither the cascade nor the history. Sixteen pooled
            // swing by one per cent.
            double pooled = 0.0;
            for (std::uint32_t frame = 0; frame < Shaders::ACCUMULATE_FRAMES; ++frame)
            {
                std::vector<float> one;
                renderFiltered(scene, camera, size, one, 1, frame);
                const double error = errorAgainstReference(one);
                pooled += error * error / static_cast<double>(Shaders::ACCUMULATE_FRAMES);
            }
            const double alone = std::sqrt(pooled);

            // Measured on this box through `Channel::Radiance`, over ten starts of the sampler's
            // stream: the cascade alone leaves 0.00475 to 0.00486 pooled over sixteen frames, and the
            // same sixteen accumulated leave 0.00268 to 0.00285 — the history removes two fifths of
            // the error the filter cannot reach, and the ratio runs from 0.556 to 0.590 with a spread
            // of 0.011 about 0.573. Deterministic to the last digit for one stream, and a different
            // stream is what any change to the sampler or the scene hands this test, so the bound
            // below sits seven spreads above the mean rather than one.
            //
            EXPECT_GT(alone, 0.003) << "the cascade alone leaves enough error here for the question to mean something: "
                                    << alone;
            EXPECT_LT(settled, alone * 0.65) << "and a history of " << Shaders::ACCUMULATE_FRAMES
                                             << " frames takes over a third of what the cascade "
                                             << "cannot: " << alone << " becomes " << settled;

            // **And it converges on the reference rather than on its own opinion.** Quieter is not
            // the claim — an average that drifted would be quieter too, and wrong.
            double settledMean = 0.0;
            double referenceMean = 0.0;
            std::size_t counted = 0;
            for (std::size_t i = 0; i < settledPixels.size(); i += 4)
                for (std::size_t channel = 0; channel < 3; ++channel)
                {
                    const std::size_t at = i + channel;
                    if (settledPixels[at] == reference[at] && reference[at] == 0.0f)
                        continue;

                    settledMean += static_cast<double>(settledPixels[at]);
                    referenceMean += static_cast<double>(reference[at]);
                    ++counted;
                }

            ASSERT_GT(counted, 0u);
            settledMean /= static_cast<double>(counted);
            referenceMean /= static_cast<double>(counted);

            EXPECT_NEAR(settledMean, referenceMean, referenceMean * 0.02)
                << "the accumulated mean is " << settledMean << " against a converged " << referenceMean;
        }
    }
}
