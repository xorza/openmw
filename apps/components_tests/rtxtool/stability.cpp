#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <osg/Group>

#include <gtest/gtest.h>

#include <boost/program_options/variables_map.hpp>

#include <components/esm3/loadcell.hpp>
#include <components/files/configurationmanager.hpp>
#include <components/rtx/camera.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>
#include <components/rtx/texturebuilder.hpp>

#include <apps/rtxtool/cellscene.hpp>
#include <apps/rtxtool/lighting.hpp>
#include <apps/rtxtool/world.hpp>

#include "../rtx/harness.hpp"
#include "installation.hpp"

namespace RtxTool
{
    namespace
    {
        namespace bpo = boost::program_options;

        /// How far a run of frames of one still camera spread around their own mean, as an RMS over
        /// `[0, 1]`.
        ///
        /// **A spread and not the difference of two frames**, because the jitter walks a Halton
        /// sequence and consecutive terms of one are not evenly spaced: which pair a two-frame
        /// measurement lands on moves the answer by a factor of two on its own. Measured over four
        /// frames on the fixture below, the two-frame form separated the correct signs from the
        /// nearest wrong ones by 1.21× and this one by 1.65×.
        double spreadOf(const std::vector<std::vector<std::uint8_t>>& frames)
        {
            double total = 0.0;
            std::size_t counted = 0;
            const double count = static_cast<double>(frames.size());

            for (std::size_t at = 0; at < frames.front().size(); at += 4)
                for (std::size_t channel = 0; channel < 3; ++channel)
                {
                    double mean = 0.0;
                    for (const std::vector<std::uint8_t>& frame : frames)
                        mean += frame[at + channel];
                    mean /= count;

                    for (const std::vector<std::uint8_t>& frame : frames)
                    {
                        const double apart = (frame[at + channel] - mean) / 255.0;
                        total += apart * apart / count;
                    }
                    ++counted;
                }

            return std::sqrt(total / static_cast<double>(counted));
        }

        /// How far apart four consecutive resolves of a still camera may be. See the sweep below.
        ///
        /// The geometric mean of what the correct signs measure and what the nearest wrong pair
        /// measures, so it fails on either axis inverted and not only on both.
        constexpr double sStillBound = 0.00227;

        /// Balmora, extracted and uploaded, or the reason there is nothing to measure.
        ///
        /// **Shared because the bounds below are calibrated against this fixture and no other.** The
        /// note on `sStillBound` is explicit that a different amount of content would be "a different
        /// fixture wearing the same number"; two copies of this setup would be two fixtures the day
        /// one of them was edited.
        struct Balmora
        {
            // **Declaration order is destruction order reversed, and that is the order this needs.**
            // The renderer holds device memory filled from the scene and reached through the world's
            // image manager, so it has to go first; putting it above either of them would tear down
            // what it was built from while it still existed.
            std::unique_ptr<World> mWorld;
            Rtx::SceneDesc mScene;
            std::unique_ptr<Rtx::Renderer> mRenderer;
            CellLighting mLighting;

            /// Non-empty where the machine cannot answer: no device, no installation, no shaders.
            std::string mObstacle;
        };

        Balmora loadBalmora(Files::ConfigurationManager& config, bpo::variables_map& variables, Rtx::Upscale upscale)
        {
            Balmora held;

            if (const std::string obstacle = Rtx::Testing::findInstanceObstacle(); !obstacle.empty())
            {
                held.mObstacle = obstacle;
                return held;
            }

            held.mWorld = openWorld(config, variables);
            if (held.mWorld == nullptr)
            {
                held.mObstacle = "no Morrowind installation is configured, and a synthetic scene cannot see this";
                return held;
            }

            // Balmora from outside, which is the harness's default exterior: buildings, terrain and
            // a horizon, all at pixel scale from here.
            const ESM::Cell* cell = held.mWorld->findCell("-3,-2");
            if (cell == nullptr)
            {
                held.mObstacle = "the configured installation has no Balmora, so it is not Morrowind";
                return held;
            }

            osg::ref_ptr<osg::Group> root = new osg::Group;
            Rtx::SceneExtractor extractor(held.mScene);
            // **One cell and not the region the harness now loads by default.** What these measure is
            // the temporal resolve, and the bounds were calibrated against exactly this much content;
            // forty-nine cells would be a different fixture wearing the same number.
            LoadedCells loaded;
            held.mLighting
                = loadRegion(*held.mWorld, *cell, *root, held.mScene, extractor, loaded, "Clear", 0, 12.0f, false)
                      .mLighting;

            // **The walk is what places anything at all.** `loadRegion` builds the graph and nothing
            // more, so without this the scene held whatever had been put into it directly — which
            // for as long as the sea was an analytic quad was exactly one quad, and this measured
            // the temporal resolve of a frame containing a single flat surface.
            extractor.extract(*root, osg::Matrixf::identity(), 0);
            if (held.mScene.getInstances().empty())
            {
                held.mObstacle = "the cell placed no geometry";
                return held;
            }

            Rtx::RendererOptions options;
            options.mShaderDirectory = Rtx::Testing::getShaderDirectory();
            options.mWidth = 1920;
            options.mHeight = 1080;
            options.mUpscale = upscale;
            options.mValidation.mEnabled = true;
            options.mValidation.mAbortOnError = false;

            std::string reason;
            held.mRenderer = Rtx::createRenderer(options, reason);
            if (held.mRenderer == nullptr)
            {
                held.mObstacle = reason;
                return held;
            }

            const Rtx::SceneTextures described(held.mScene, held.mWorld->getImageManager());
            held.mRenderer->setScene(Rtx::sWorld, held.mScene, described.getDescriptions(), Rtx::SeaState{});

            return held;
        }

        /// A still camera over real content, and whether the resolve settles.
        ///
        /// **The one fault in this integration that nothing else can see.** The jitter handed to NGX
        /// is the offset applied to the *projection*; the trace's own is the offset applied to the
        /// sample *coordinate* — the same picture reached from opposite directions, so one is the
        /// negative of the other. Invert it and Ray Reconstruction un-jitters the way that doubles
        /// the offset instead of cancelling it. The feature still builds, still evaluates, still
        /// returns success, the validation layers still say nothing, and every quality figure taken
        /// from a single frame still looks right. The image just shakes about a pixel a frame.
        ///
        /// A camera that does not move turns that into a number. Sweeping the four sign combinations
        /// over this view, four frames each, DLAA:
        ///
        /// | `Jitter.Offset` | spread |
        /// |---|---|
        /// | `-x, -y` (what the code does) | 0.00094 |
        /// | `+x, -y` | 0.00546 |
        /// | `-x, +y` | 0.00639 |
        /// | `+x, +y` | 0.00953 |
        ///
        /// Which is `sqrt(0.00094 * 0.00546) = 0.00227` for the bound, 2.4× above what the correct
        /// signs measure and 2.4× below the nearest wrong pair.
        ///
        /// **Re-measured, and the whole sweep moved.** The figures here were 0.00364, 0.00602,
        /// 0.00835 and 0.01055 while the colour-pair guides were wired; dropping them — see the
        /// comment in `dlsspass.cpp` for why — took the correct combination to a quarter of what it
        /// had been and left the old bound with five times the headroom it was calibrated for. A
        /// bound that no longer sits between the two things it separates is a test that has stopped
        /// being one, so it moves when the sweep does.
        ///
        /// **DLAA and not an upscaling preset**, so what is measured is the temporal resolve alone
        /// rather than the resolve plus a reconstruction from a quarter of the pixels.
        ///
        /// **Real content, and a synthetic fixture will not do.** Two were tried and neither could
        /// fail on the thing this exists for: a wall carrying a full-frequency noise texture measured
        /// 0.0145, 0.0160, 0.0176 and 0.0142 for the four combinations — no separation, and the
        /// wrong signs lowest — and a field of five hundred small quads at staggered depths against
        /// a bright sky, which is silhouette edges everywhere, measured 0.0062, 0.0065, 0.0064 and
        /// 0.0069. A check that cannot fail on its own subject is worse than an honest skip.
        ///
        /// A frame that is black is also a still one, so this asserts the picture is lit first.
        TEST(RtxUpscalerStabilityTest, aStillCameraResolvesToAStillPicture)
        {
            Files::ConfigurationManager config;
            bpo::variables_map variables;
            Balmora held = loadBalmora(config, variables, Rtx::Upscale::Dlaa);
            if (!held.mObstacle.empty())
                GTEST_SKIP() << held.mObstacle;

            const std::unique_ptr<Rtx::Renderer>& renderer = held.mRenderer;
            const CellLighting& lighting = held.mLighting;

            const Rtx::FrameExtents extents = renderer->getExtents();
            ASSERT_EQ(extents.mRenderWidth, extents.mOutputWidth) << "DLAA is one to one, or this measures upscaling";

            const osg::Vec3f origin(-19216.0f, -14896.0f, 160.0f);
            const osg::Vec3f target(-19424.0f, -12960.0f, 60.0f);
            Rtx::Shaders::VisibilityConstants camera
                = Rtx::makeCamera(origin, target, 60.0f, extents.mRenderWidth, extents.mRenderHeight, 100000.0f);
            applyLighting(lighting, camera);

            // Sixteen, because the first is a reset and the ones after it are the history filling.
            // The last four are what is measured, by which point the resolve has settled into
            // whatever it is going to do.
            constexpr std::uint32_t sFrames = 16;
            constexpr std::size_t sMeasured = 4;
            std::vector<std::vector<std::uint8_t>> tail;

            for (std::uint32_t frame = 0; frame < sFrames; ++frame)
            {
                camera.mFrame = frame;
                renderer->renderFrame(camera, Rtx::FrameOptions{});

                if (frame + sMeasured >= sFrames)
                {
                    tail.emplace_back();
                    renderer->readPixels(tail.back());
                }
            }

            ASSERT_EQ(tail.size(), sMeasured);
            ASSERT_EQ(tail.front().size(), std::size_t{ extents.mOutputWidth } * extents.mOutputHeight * 4);

            double lit = 0.0;
            for (std::size_t at = 0; at < tail.back().size(); at += 4)
                lit += tail.back()[at];
            lit /= static_cast<double>(tail.back().size()) / 4.0;

            EXPECT_GT(lit, 8.0) << "the frame is black, so holding still proves nothing";

            const double spread = spreadOf(tail);
            std::cout << "upscaler spread over the last " << sMeasured << " frames: " << spread << std::endl;
            EXPECT_LT(spread, sStillBound)
                << "the resolve is not settling, which is what an inverted jitter offset looks like";

            std::vector<std::string> errors;
            renderer->takeValidationErrors(errors);
            for (const std::string& error : errors)
                ADD_FAILURE() << "validation error from the stability run: " << error;
        }
    }
}
