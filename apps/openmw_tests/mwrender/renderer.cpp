#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <MyGUI_ITexture.h>
#include <osg/Group>
#include <osg/Image>
#include <osg/Timer>
#include <osg/ref_ptr>

#include <apps/openmw/mwrender/ground.hpp>
#include <apps/openmw/mwrender/mapoverlay.hpp>
#include <apps/openmw/mwrender/offscreenview.hpp>
#include <apps/openmw/mwrender/renderer.hpp>
#include <apps/openmw/mwrender/rendermode.hpp>
#include <components/misc/frameclock.hpp>
#include <components/myguiplatform/myguiplatform.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/sdlutil/vsyncmode.hpp>
#include <components/vfs/pathutil.hpp>

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

            /// The limit the seam kept, at each `applyFrameRateLimit`.
            std::vector<float> mLimits;

            /// Whether `holdFrame` holds the frame itself, as a driver that paces does, and for how
            /// long.
            bool mHolds = false;
            std::chrono::milliseconds mHold{ 0 };

            using Renderer::getLastHold;

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
            void applyFrameRateLimit() override { mLimits.push_back(getFrameRateLimit()); }

            bool holdFrame() override
            {
                if (mHolds)
                    std::this_thread::sleep_for(mHold);
                return mHolds;
            }
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

        /// **The default `awaitFrame` is the limiter the engine's loop used to hold**: it sleeps to
        /// the limit and answers the limit's own length where it slept, and the wall where it did
        /// not. The rasterizer keeps exactly the pacing it had, one call earlier in the loop. A
        /// renderer that paces its own frames hears of each limit once, with the limit already
        /// kept.
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

            EXPECT_EQ(renderer.mLimits, (std::vector<float>{ 200.0f, 0.0f }));
        }

        /// **A nested frame is opened as the loop opens its own**: held to the limit, and a wall
        /// clock moved on by what it stood for, so a loading screen or a message box stamps and
        /// steps the interface by its own time and not the outer frame's. At 200 frames a second the
        /// hold is 5 ms, which is the step the frame answers and the clock takes.
        ///
        /// **A clock that states its step moves by the loop's frames alone**: a nested frame of a
        /// measured run stands for nothing and leaves the clock where the loop put it, because how
        /// many a run draws is the wall's answer.
        TEST(RendererTest, aNestedFrameStepsAWallClockAndLeavesAStatedOne)
        {
            RecordingRenderer renderer;
            renderer.setFrameRateLimit(200.0f);

            Misc::FrameClock wall;
            renderer.setFrameClock(wall);
            renderer.awaitFrame();
            EXPECT_FLOAT_EQ(renderer.openNestedFrame(), 0.005f);
            EXPECT_DOUBLE_EQ(wall.getStep(), 0.005);

            Misc::FrameClock stated(1.0f / 60.0f);
            renderer.setFrameClock(stated);
            stated.advance(0.0);
            const double now = stated.getNow();
            EXPECT_EQ(renderer.openNestedFrame(), 0.0f);
            EXPECT_EQ(stated.getNow(), now) << "a nested frame moved a clock whose steps are the loop's";
        }

        /// **A window is sized so that its pixels are the resolution asked for, at any scale.** Asked
        /// for 3840 by 2160 on a display at one and a half, a window made at that many points comes
        /// out 5760 by 3240 pixels, and the size that gives the pixels asked for is 3840 × 3840 /
        /// 5760 = 2560 by 1440 points. Upstream's `3840 / (5760 / 3840)` divided in whole numbers,
        /// came to 3840 again, and left the window at one and a half times the resolution. At a
        /// scale of two the two agree, and at one there is nothing to fit.
        TEST(RendererTest, aWindowIsFittedSoItsPixelsAreTheResolutionAsked)
        {
            const WindowPlacement asked{ .mWidth = 3840, .mHeight = 2160 };

            EXPECT_EQ(asked.fittedSize(osg::Vec2i(3840, 2160), osg::Vec2i(5760, 3240)), osg::Vec2i(2560, 1440));
            EXPECT_EQ(asked.fittedSize(osg::Vec2i(3840, 2160), osg::Vec2i(7680, 4320)), osg::Vec2i(1920, 1080));
            EXPECT_EQ(asked.fittedSize(osg::Vec2i(3840, 2160), osg::Vec2i(3840, 2160)), osg::Vec2i(3840, 2160));
        }

        /// **One opening for both holds.** Held by the renderer for three frames, by the limiter
        /// for three and by the renderer for three again, every interval `awaitFrame` answers is
        /// the wall from the frame before to this one. With a clock of each hold's own, the first
        /// frame after a switch stood for the whole of the other hold's run: 60 ms here, where it
        /// should stand for 10 or 20. What the renderer held is the hold `getLastHold` says.
        TEST(RendererTest, anIntervalAfterASwitchOfHoldStandsForOneFrame)
        {
            using Clock = std::chrono::steady_clock;

            RecordingRenderer renderer;
            renderer.setFrameRateLimit(0.0f);
            renderer.mHold = std::chrono::milliseconds(10);

            renderer.awaitFrame();
            Clock::time_point last = Clock::now();
            for (const bool holds : { true, true, true, false, false, false, true, true, true })
            {
                renderer.mHolds = holds;

                // The frame's own work, which the interval has inside it whichever hold follows.
                std::this_thread::sleep_for(std::chrono::milliseconds(10));

                const Clock::duration stood = renderer.awaitFrame();
                const Clock::time_point now = Clock::now();
                const Clock::duration wall = now - last;
                last = now;

                // The seam's clock and this one are read a few instructions apart.
                EXPECT_LT(std::chrono::abs(stood - wall), std::chrono::milliseconds(1))
                    << (holds ? "held by the renderer" : "held by the limiter");
                if (holds)
                {
                    EXPECT_GE(renderer.getLastHold(), std::chrono::milliseconds(10));
                }
            }
        }

        /// A name this build has no renderer for is a configuration mistake, refused by name.
        TEST(RendererTest, anUnknownRendererIsRefusedByName)
        {
            EXPECT_THROW(createRenderer("software", RendererSpec{}), std::runtime_error);
        }
    }
}
