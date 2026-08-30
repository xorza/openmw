#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/camera.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/scenedesc.hpp>

#include "harness.hpp"

namespace Rtx
{
    namespace
    {
        constexpr std::uint32_t sSize = 64;
        constexpr std::array<std::uint32_t, 6> sQuadIndices{ 0, 1, 2, 0, 2, 3 };

        /// A square across the view, far enough away to fill the frame.
        const std::array<osg::Vec3f, 4> sWallCorners{
            osg::Vec3f(-500.0f, 200.0f, -500.0f),
            osg::Vec3f(500.0f, 200.0f, -500.0f),
            osg::Vec3f(500.0f, 200.0f, 500.0f),
            osg::Vec3f(-500.0f, 200.0f, 500.0f),
        };

        /// That wall, on its own, as a scene.
        SceneDesc wall()
        {
            SceneDesc scene;
            scene.addInstance(MeshInstance{
                .mTransform = osg::Matrixf::identity(), .mMesh = scene.addMesh(sWallCorners, {}, {}, sQuadIndices) });

            return scene;
        }

        bool reports(std::span<const GpuSpan> spans, std::string_view name)
        {
            return std::any_of(spans.begin(), spans.end(), [&](const GpuSpan& span) { return span.mName == name; });
        }

        double totalOf(std::span<const GpuSpan> spans)
        {
            double sum = 0.0;
            for (const GpuSpan& span : spans)
                sum += span.mMs;

            return sum;
        }

        /// One frame's result with its zones copied out, because the span the renderer hands back
        /// is overwritten by the frame after next.
        struct Drawn
        {
            std::uint32_t mHits = 0;
            /// From before the submit to after the wait: the whole of what the device did, and the
            /// CPU sat through, for this frame.
            double mWallMs = 0.0;
            std::vector<GpuSpan> mGpu;
        };

        /// Draws one frame and waits for it, so what comes back is that frame's own report.
        Drawn draw(Renderer& renderer, const Shaders::VisibilityConstants& camera)
        {
            const auto start = std::chrono::steady_clock::now();
            renderer.renderFrame(camera, FrameOptions{});
            const std::optional<FrameResult> result = renderer.finishFrame();
            const double wallMs
                = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

            EXPECT_TRUE(result.has_value()) << "a frame was submitted and nothing came back";
            if (!result.has_value())
                return Drawn{};

            return Drawn{ .mHits = result->mHits,
                .mWallMs = wallMs,
                .mGpu = std::vector<GpuSpan>(result->mGpu.begin(), result->mGpu.end()) };
        }

        /// A frame accounts for its own device time, pass by pass.
        ///
        /// **The thing a wall clock around the submit cannot do.** One `renderFrame` is a trace, a
        /// wavelet, a composite and two tone passes, and the CPU sees one number for all of them.
        /// What is asserted is that each is measured separately, that each is a real duration, and
        /// that together they fit inside the submit that contained them — which is the cross-check
        /// that says these are the device's clock and not something invented.
        TEST(RtxGpuTimerTest, aFrameAccountsForItsOwnDeviceTimePassByPass)
        {
            std::string reason;
            Renderer* renderer = Testing::getRenderer(reason);
            if (renderer == nullptr)
                GTEST_SKIP() << reason;

            renderer->resize(sSize, sSize);

            SceneDesc scene = wall();
            renderer->setScene(Rtx::sWorld, scene, {}, SeaState{});

            const Shaders::VisibilityConstants camera
                = makeCamera(osg::Vec3f(), osg::Vec3f(0.0f, 100.0f, 0.0f), 60.0f, sSize, sSize, 10000.0f);

            const Drawn drawn = draw(*renderer, camera);
            if (drawn.mGpu.empty())
                GTEST_SKIP() << "this device cannot write timestamps";

            // The passes every frame records, whatever it is drawing. `filter` is here too — the
            // shared renderer does not upscale, so the wavelet runs — and is left out of the list
            // because a build without it is not a failure of this.
            for (const char* const pass : { "trace", "composite", "exposure", "tone" })
                EXPECT_TRUE(reports(drawn.mGpu, pass)) << "no zone called " << pass;

            // **And the sea is not among them where the frame has none.** `makeCamera` names no
            // water, so nothing can sample the wave tiles and nothing should synthesise them; a
            // frame that does name a level pays for them once, before the trace.
            EXPECT_FALSE(reports(drawn.mGpu, "waves")) << "a dry frame synthesised the sea";

            Shaders::VisibilityConstants flooded = camera;
            flooded.mWaterLevel = 0.0f;
            const Drawn wet = draw(*renderer, flooded);
            EXPECT_TRUE(reports(wet.mGpu, "waves")) << "a frame with water in it synthesised no sea";
            EXPECT_EQ(wet.mGpu.front().mName, "waves")
                << "the sea was synthesised somewhere other than before the trace";

            for (const GpuSpan& span : drawn.mGpu)
            {
                EXPECT_GT(span.mMs, 0.0) << span.mName << " took no time at all";
                EXPECT_LT(span.mMs, 1000.0) << span.mName << " took a second, which is a clock read wrong";
            }

            // **The containment check, which is what makes these numbers rather than noise.** Every
            // zone was recorded inside the submit `draw` waited out, and the zones do not overlap —
            // so their sum is device work the CPU also sat through, and the CPU also paid for the
            // submit itself.
            EXPECT_LT(totalOf(drawn.mGpu), drawn.mWallMs)
                << "the passes add up to more device time than the frame that held them took";

            // **A frame that placed the world says so, and one that did not, does not.** The
            // structure builds happen in submits of their own before the frame's, and the whole
            // point of carrying them in the same report is that they are the same frame's cost.
            EXPECT_FALSE(reports(drawn.mGpu, "tlas")) << "nothing was placed, so nothing was built";

            renderer->placeScene(Rtx::sWorld, scene, SeaState{});
            const Drawn placed = draw(*renderer, camera);

            EXPECT_TRUE(reports(placed.mGpu, "tlas")) << "the top level was rebuilt and went unmeasured";
            EXPECT_GT(placed.mGpu.size(), drawn.mGpu.size()) << "placing the world added no zone";

            // First, because it happened first: the order is the order the work was recorded, which
            // is what lets a reader see the frame rather than a bag of numbers.
            EXPECT_EQ(placed.mGpu.front().mName, "tlas");

            // And the report does not accumulate: the frame after is its own again.
            const Drawn after = draw(*renderer, camera);
            EXPECT_EQ(after.mGpu.size(), drawn.mGpu.size()) << "last frame's zones were carried into this one";
            EXPECT_FALSE(reports(after.mGpu, "tlas"));

            // **A cell arriving says so too, and that is the frame worth having a figure for.** The
            // structures its meshes bring are recorded ahead of the placement and ride its submit,
            // so without a bracket of their own they are device time the frame's fence carries and
            // no zone accounts for — which is exactly the frame a player feels.
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::translate(0.0f, -50.0f, 0.0f),
                .mMesh = scene.addMesh(sWallCorners, {}, {}, sQuadIndices) });

            renderer->extendScene(Rtx::sWorld, scene, {}, SeaState{});
            const Drawn arrived = draw(*renderer, camera);

            EXPECT_TRUE(reports(arrived.mGpu, "blas")) << "a mesh arrived and its structure was built unmeasured";

            // First, because the builds run before the top level that names what they built, and a
            // duration rather than a bracket that closed on itself.
            EXPECT_EQ(arrived.mGpu.front().mName, "blas");
            EXPECT_GT(arrived.mGpu.front().mMs, 0.0) << "the arrival's builds took no time at all";

            // And only on the frame the arrival landed in.
            const Drawn settled = draw(*renderer, camera);
            EXPECT_FALSE(reports(settled.mGpu, "blas")) << "nothing arrived, so nothing was built";
        }
    }
}
