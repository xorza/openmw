#include <cstdint>
#include <optional>
#include <stdexcept>

#include <gtest/gtest.h>

#include <components/rtx/reconstruction.hpp>
#include <components/rtx/renderer.hpp>

#include "apps/openmw/mwrender/renderer.hpp"
#include "apps/openmw/mwrender/rtx/rtxrun.hpp"

namespace MWRender
{
    namespace
    {
        /// A run that answers nothing, because the test never gets as far as asking it anything.
        class NoRun final : public RtxRun
        {
        public:
            bool isHeadless() const override { return true; }
            const Rtx::ValidationOptions& getValidation() const override { return mValidation; }
            bool wantsHitCounts() const override { return false; }
            std::optional<float> getStep() const override { return std::nullopt; }
            std::optional<bool> getSettled() const override { return std::nullopt; }
            std::optional<std::uint32_t> getSampleFrame() const override { return std::nullopt; }
            std::uint32_t getAccumulated() const override { return 0; }
            bool wantsSecondWalk() const override { return false; }
            void beforeFrame() override {}
            void frame(const FrameContext&, const FrameReport&) override {}

        private:
            Rtx::ValidationOptions mValidation;
        };

        /// **A run installed for the rasterizer is refused before a window is made.** The choice
        /// of renderer is the setting's, and a harness that installed a run without setting it
        /// has contradicted itself; the rasterizer ignoring the run would answer that with silence.
        TEST(RendererTest, aRunInstalledForTheRasterizerIsRefusedByName)
        {
            NoRun run;
            const RtxSetup setup{ .mProfile = Rtx::RenderProfile{}, .mRun = run };
            const RendererSpec spec{ .mRtx = &setup };

            EXPECT_THROW(createRenderer("opengl", spec), std::runtime_error);
        }

        /// A name this build has no renderer for is a configuration mistake, refused by name.
        TEST(RendererTest, anUnknownRendererIsRefusedByName)
        {
            EXPECT_THROW(createRenderer("software", RendererSpec{}), std::runtime_error);
        }
    }
}
