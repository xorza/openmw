#include <algorithm>
#include <array>
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

        /// A wall across the view, far enough away to fill the frame.
        SceneDesc wall()
        {
            const std::array corners{
                osg::Vec3f(-500.0f, 200.0f, -500.0f),
                osg::Vec3f(500.0f, 200.0f, -500.0f),
                osg::Vec3f(500.0f, 200.0f, 500.0f),
                osg::Vec3f(-500.0f, 200.0f, 500.0f),
            };

            SceneDesc scene;
            scene.addInstance(MeshInstance{
                .mTransform = osg::Matrixf::identity(), .mMesh = scene.addMesh(corners, {}, {}, sQuadIndices) });

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

            const SceneDesc scene = wall();
            renderer->setScene(Rtx::sWorld, scene, {}, SeaState{});

            const Shaders::VisibilityConstants camera
                = makeCamera(osg::Vec3f(), osg::Vec3f(0.0f, 100.0f, 0.0f), 60.0f, sSize, sSize, 10000.0f);

            const FrameResult drawn = renderer->renderFrame(camera, FrameOptions{});
            if (drawn.mGpu.empty())
                GTEST_SKIP() << "this device cannot write timestamps";

            // The passes every frame records, whatever it is drawing. `filter` is here too — the
            // shared renderer does not upscale, so the wavelet runs — and is left out of the list
            // because a build without it is not a failure of this.
            for (const char* const pass : { "trace", "composite", "exposure", "tone" })
                EXPECT_TRUE(reports(drawn.mGpu, pass)) << "no zone called " << pass;

            for (const GpuSpan& span : drawn.mGpu)
            {
                EXPECT_GT(span.mMs, 0.0) << span.mName << " took no time at all";
                EXPECT_LT(span.mMs, 1000.0) << span.mName << " took a second, which is a clock read wrong";
            }

            // **The containment check, which is what makes these numbers rather than noise.** Every
            // zone was recorded inside the one submit `mTraceMs` waited on, and the zones do not
            // overlap — so their sum is device work the CPU also sat through, and the CPU also paid
            // for the submit itself.
            EXPECT_LT(totalOf(drawn.mGpu), drawn.mTraceMs)
                << "the passes add up to more device time than the submit that held them took";

            // **A frame that placed the world says so, and one that did not, does not.** The
            // structure builds happen in submits of their own before the frame's, and the whole
            // point of carrying them in the same report is that they are the same frame's cost.
            EXPECT_FALSE(reports(drawn.mGpu, "tlas")) << "nothing was placed, so nothing was built";

            renderer->placeScene(Rtx::sWorld, scene, SeaState{});
            const FrameResult placed = renderer->renderFrame(camera, FrameOptions{});

            EXPECT_TRUE(reports(placed.mGpu, "tlas")) << "the top level was rebuilt and went unmeasured";
            EXPECT_GT(placed.mGpu.size(), drawn.mGpu.size()) << "placing the world added no zone";

            // First, because it happened first: the order is the order the work was recorded, which
            // is what lets a reader see the frame rather than a bag of numbers.
            EXPECT_EQ(placed.mGpu.front().mName, "tlas");

            // And the report does not accumulate: the frame after is its own again.
            const FrameResult after = renderer->renderFrame(camera, FrameOptions{});
            EXPECT_EQ(after.mGpu.size(), drawn.mGpu.size()) << "last frame's zones were carried into this one";
            EXPECT_FALSE(reports(after.mGpu, "tlas"));
        }
    }
}
