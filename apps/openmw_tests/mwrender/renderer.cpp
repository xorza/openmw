#include <chrono>
#include <filesystem>
#include <memory>
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
#include <components/sdlutil/vsyncmode.hpp>
#include <components/vfs/pathutil.hpp>

#include "apps/openmw/mwrender/ground.hpp"
#include "apps/openmw/mwrender/mapoverlay.hpp"
#include "apps/openmw/mwrender/offscreenview.hpp"
#include "apps/openmw/mwrender/renderer.hpp"
#include "apps/openmw/mwrender/rendermode.hpp"

namespace MWRender
{
    namespace
    {
        /// A renderer that draws nothing and records what the seam tells it about the world.
        class RecordingRenderer final : public Renderer
        {
        public:
            /// Whether the world was to be drawn, at each `applyWorldShown`.
            std::vector<bool> mApplied;

            void configureResources(Resource::ResourceSystem&) override {}
            SDL_Window* getWindow() const override { return nullptr; }
            std::unique_ptr<Ground> createGround(const GroundSpec&) override { return nullptr; }
            float getGroundReach() const override { return 0.0f; }
            osg::ref_ptr<osg::Group> createSceneRoot() override { return new osg::Group; }
            void attachWorld(RenderingManager&, osg::Group&) override {}
            void advance(double) override {}
            void eventTraversal() override {}
            void updateTraversal() override {}
            void renderFrame(const SceneFrame&) override {}
            std::unique_ptr<OffscreenView> createWorldView(const OffscreenViewSpec&) override { return nullptr; }
            std::unique_ptr<SubjectView> createSubjectView(const OffscreenViewSpec&) override { return nullptr; }
            std::unique_ptr<MapOverlay> createMapOverlay(const MapOverlaySpec&) override { return nullptr; }
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
            void applyViewMask() override {}
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

        /// **The default `awaitFrame` is the limiter the engine's loop used to hold**: it sleeps
        /// to the limit and answers the limit's own length where it slept, and the wall where it
        /// did not. The rasterizer keeps exactly the pacing it had, one call earlier in the loop.
        TEST(RendererTest, theDefaultAwaitFrameSleepsToTheLimitAndAnswersIt)
        {
            using Clock = std::chrono::steady_clock;

            RecordingRenderer renderer;
            renderer.setFrameRateLimit(200.0f);
            const Clock::duration limit
                = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(1.0f / 200.0f));

            renderer.awaitFrame();
            const Clock::time_point began = Clock::now();
            const Clock::duration stood = renderer.awaitFrame();
            const Clock::duration slept = Clock::now() - began;
            EXPECT_EQ(stood, limit) << "a frame it slept for stood for the limit, as the limiter answers";
            EXPECT_GE(slept, std::chrono::milliseconds(4)) << "and it slept for it";

            renderer.setFrameRateLimit(0.0f);
            renderer.awaitFrame();
            const Clock::time_point again = Clock::now();
            const Clock::duration free = renderer.awaitFrame();
            EXPECT_LT(Clock::now() - again, std::chrono::milliseconds(2)) << "no limit is no sleep";
            EXPECT_LT(free, std::chrono::milliseconds(2)) << "and the wall is what stood";
        }

        /// A name this build has no renderer for is a configuration mistake, refused by name.
        TEST(RendererTest, anUnknownRendererIsRefusedByName)
        {
            EXPECT_THROW(createRenderer("software", RendererSpec{}), std::runtime_error);
        }
    }
}
