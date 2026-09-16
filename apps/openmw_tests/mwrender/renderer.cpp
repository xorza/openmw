#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include <MyGUI_ITexture.h>
#include <osg/Group>
#include <osg/Image>
#include <osg/Timer>
#include <osg/ref_ptr>

#include <components/myguiplatform/myguiplatform.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/rtx/reconstruction.hpp>
#include <components/rtx/renderer.hpp>
#include <components/sdlutil/vsyncmode.hpp>
#include <components/vfs/pathutil.hpp>

#include "apps/openmw/mwrender/ground.hpp"
#include "apps/openmw/mwrender/offscreenview.hpp"
#include "apps/openmw/mwrender/renderer.hpp"
#include "apps/openmw/mwrender/rendermode.hpp"
#include "apps/openmw/mwrender/rtx/rtxrun.hpp"

namespace MWRender
{
    namespace
    {
        /// A run that answers nothing, because the test never gets as far as asking it anything.
        class NoRun final : public RtxRun
        {
        public:
            std::optional<std::uint32_t> getSampleFrame() const override { return std::nullopt; }
            std::uint32_t getAccumulated() const override { return 0; }
            bool wantsSecondWalk() const override { return false; }
            bool wantsFrameCopy() const override { return false; }
            void beforeFrame() override {}
            void frame(const FrameContext&, const FrameReport&) override {}
        };

        /// A renderer that draws nothing and records what the seam tells it about the world.
        class RecordingRenderer final : public Renderer
        {
        public:
            /// Whether the world was to be drawn, at each `applyWorldShown`.
            std::vector<bool> mApplied;

            void prepareResources(Resource::ResourceSystem&) override {}
            SDL_Window* getWindow() const override { return nullptr; }
            Ground createGround(const GroundSpec&) override { return {}; }
            float getGroundReach() const override { return 0.0f; }
            osg::ref_ptr<osg::Group> createSceneRoot() override { return new osg::Group; }
            void attachWorld(RenderingManager&, osg::Group&) override {}
            void advance(double) override {}
            void eventTraversal() override {}
            void updateTraversal() override {}
            void renderFrame(const SceneFrame&) override {}
            std::unique_ptr<OffscreenView> createWorldView(const OffscreenViewSpec&) override { return nullptr; }
            std::unique_ptr<SubjectView> createSubjectView(const OffscreenViewSpec&) override { return nullptr; }
            MyGUI::ITexture& freezeFrame() override { throw std::logic_error("not asked"); }
            void renderGui() override {}
            void capture(osg::Image&, int, int) override {}
            void saveScreenshot() override {}
            void setVSync(SDLUtil::VSyncMode) override {}
            osg::Timer_t getStartTick() const override { return 0; }
            std::unique_ptr<MyGUIPlatform::Platform> createGuiPlatform(
                float, VFS::Path::NormalizedView, const std::filesystem::path&) override
            {
                return nullptr;
            }

        protected:
            void adoptTraversalRoot(osg::Group&) override {}
            void applyViewMask(unsigned int) override {}
            void applyWorldShown() override { mApplied.push_back(drawsWorld()); }
        };

        /// **A cover that ends while `tws` is off leaves the world hidden, and `tws` under a cover
        /// brings nothing back.** The two are one answer to a frame and two to the game, and the
        /// renderer hears about each change once: the window manager asks every frame.
        TEST(RendererTest, aCoverAndTwsAreTwoReasonsAndOneAnswer)
        {
            RecordingRenderer renderer;
            EXPECT_TRUE(renderer.drawsWorld());

            renderer.showWorld(false);
            renderer.showWorld(false);
            EXPECT_FALSE(renderer.isWorldShown());
            EXPECT_TRUE(renderer.isWorldToggled());
            EXPECT_EQ(renderer.mApplied, (std::vector<bool>{ false }));

            EXPECT_FALSE(renderer.toggleRenderMode(Render_Scene));
            EXPECT_FALSE(renderer.isWorldToggled());
            EXPECT_EQ(renderer.mApplied, (std::vector<bool>{ false, false }));

            renderer.showWorld(true);
            EXPECT_TRUE(renderer.isWorldShown());
            EXPECT_FALSE(renderer.drawsWorld());
            EXPECT_EQ(renderer.mApplied, (std::vector<bool>{ false, false, false }));

            EXPECT_TRUE(renderer.toggleRenderMode(Render_Scene));
            EXPECT_TRUE(renderer.drawsWorld());
            EXPECT_EQ(renderer.mApplied, (std::vector<bool>{ false, false, false, true }));

            // The rest are the game's own nodes, and a renderer that has none says so.
            EXPECT_FALSE(renderer.toggleRenderMode(Render_Wireframe));
            EXPECT_EQ(renderer.mApplied.size(), 4u);
        }

        /// **A run installed for the rasterizer is refused before a window is made.** The choice
        /// of renderer is the setting's, and a harness that installed a run without setting it
        /// has contradicted itself; the rasterizer ignoring the run would answer that with silence.
        TEST(RendererTest, aRunInstalledForTheRasterizerIsRefusedByName)
        {
            NoRun run;
            const RtxSetup setup{ .mSetup = {}, .mRun = run };
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
