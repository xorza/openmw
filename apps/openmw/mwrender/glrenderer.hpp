#ifndef GAME_RENDER_GLRENDERER_H
#define GAME_RENDER_GLRENDERER_H

#include <fstream>
#include <memory>

#include <osg/ref_ptr>

#include "renderer.hpp"

namespace osgViewer
{
    class ScreenCaptureHandler;
    class Viewer;
}

namespace Resource
{
    class ResourceSystem;
}

namespace MyGUIPlatform
{
    class OSGTexture;
}

namespace osg
{
    class Texture2D;
}

namespace SDLUtil
{
    class GraphicsWindowSDL2;
}

namespace VFS
{
    class Manager;
}

namespace SceneUtil
{
    class AsyncScreenCaptureOperation;
    class LightManager;
    class SelectDepthFormatOperation;

    namespace Color
    {
        class SelectColorFormatOperation;
    }
}

namespace Stereo
{
    class Manager;
}

namespace MWRender
{
    class CopyFramebufferToTextureCallback;
    class GlWorld;
    class PostProcessor;
    class ScreenshotManager;

    /// The picture as OpenSceneGraph draws it: a GL window, a viewer and upstream's frame loop.
    ///
    /// **The rasterizer is not modified, wrapped or conditionally compiled around — it is gathered.**
    /// Every threading, realize and traversal decision here is upstream's, moved rather than
    /// rewritten, which is what makes "does the other renderer do this correctly" answerable by
    /// comparison (`CLAUDE.md`).
    class GlRenderer final : public Renderer
    {
    public:
        explicit GlRenderer(const RendererSpec& spec);
        ~GlRenderer() override;

        void prepareResources(Resource::ResourceSystem& resources) override;

        float getGroundReach() const override;
        SDL_Window* getWindow() const override { return mWindow; }

        osg::ref_ptr<osg::Group> createSceneRoot() override;
        void attachWorld(RenderingManager& world, osg::Group& worldRoot) override;
        void detachWorld() override;
        void adoptTraversalRoot(osg::Group& root) override;
        void applyViewMask(unsigned int mask) override;
        void showWorld(bool shown) override;
        bool toggleRenderMode(RenderMode mode) override;

        PostProcessor* getPostProcessor() override;

        void advance(double simulationTime) override;
        void eventTraversal() override;
        void updateTraversal() override;
        void describeFrame(const SceneFrame& frame) override;
        void renderFrame(const SceneFrame& frame) override;

        Ground createGround(const GroundSpec& spec) override;
        std::unique_ptr<OffscreenView> createWorldView(const OffscreenViewSpec& spec) override;
        std::unique_ptr<SubjectView> createSubjectView(const OffscreenViewSpec& spec) override;

        MyGUI::ITexture& freezeFrame() override;

        void renderGui() override;
        void beginLoading() override;
        void endLoading() override;
        void applyLoadingBudget(double targetFrameRate) override;

        void capture(osg::Image& image, int width, int height) override;
        void setScreenshotWriter(SceneUtil::AsyncScreenCaptureOperation& writer) override;
        void saveScreenshot() override;

        void suspendDraw() override;
        void resumeDraw() override;

        osgUtil::IncrementalCompileOperation* getCompileOperation() const override;

        void setVSync(SDLUtil::VSyncMode mode) override;
        void processChangedSettings(const Settings::CategorySettingVector& changed) override;

        void addCell(const MWWorld::CellStore* cell) override;
        void removeCell(const MWWorld::CellStore* cell) override;
        void addWaterRippleEmitter(const MWWorld::Ptr& ptr) override;
        void removeWaterRippleEmitter(const MWWorld::Ptr& ptr) override;
        void emitWaterRipple(const osg::Vec3f& position) override;
        void notifyWorldSpaceChanged() override;
        void listAssetsToPreload(
            std::vector<VFS::Path::Normalized>& models, std::vector<VFS::Path::Normalized>& textures) override;

        std::unique_ptr<MyGUIPlatform::Platform> createGuiPlatform(
            float scalingFactor, VFS::Path::NormalizedView resourcePath, const std::filesystem::path& logPath) override;

        osg::Timer_t getStartTick() const override;

        /// Into the viewer's queue, for the handlers it carries: the stats overlay's keys, and
        /// the resize its own handlers reposition by. The graphics context follows the window too.
        void beginEvents() override;
        void functionKey(int index, bool pressed) override;
        void windowResized(int x, int y, int width, int height) override;

    private:
        /// The overlay the debug keys toggle, and the per-frame dump `OPENMW_OSG_STATS_FILE` asks
        /// for. Both are OSG's, so both are this renderer's to install and to write.
        void installStatsOverlay(const VFS::Manager& vfs);

        /// One frame of the dump, into `mStatsFile`.
        void reportStats(unsigned frameNumber);

        /// Makes the SDL window and the OpenGL context in it, retrying at half the antialiasing
        /// each time the driver refuses. Upstream's loop, unchanged.
        void createWindow();

        /// Spreads the compiling of what a loader hands over across frames.
        ///
        /// **Decided here, because whether there is anything to compile is this renderer's own
        /// question**: a ray tracer builds no OpenGL objects and keeps none.
        void compileIncrementally();

        int mMaxTextureUnits = 0;

        /// Where `OPENMW_OSG_STATS_FILE` is written, or closed where nothing asked for it.
        std::ofstream mStatsFile;

        /// How many frames behind the dump runs: the draw thread reports a frame's figures after the
        /// main thread has moved on, and three frames is where they have all landed.
        static constexpr unsigned sStatsReportDelay = 3;

        /// What the scene root, the GUI and an offscreen view's light rig are built out of. Known
        /// from `prepareResources` onwards, which is before any of them asks.
        Resource::ResourceSystem* mResources = nullptr;

        SDL_Window* mWindow = nullptr;

        /// Held so the destructor can let the GL context go while the window it is bound to still
        /// exists. The base holds the camera and is destroyed last, so releasing the viewer does
        /// not on its own release what the camera points at.
        osg::ref_ptr<SDLUtil::GraphicsWindowSDL2> mGraphicsWindow;

        osg::ref_ptr<osgViewer::Viewer> mViewer;

        /// What the update traversal was set to before the world was hidden. Restored rather than
        /// defaulted, because somebody else chose it; the cull comes back from the seam's view mask.
        unsigned int mShownUpdateMask = 0;

        /// What the compiler was given per frame before a loading screen took the whole of it.
        double mLoadingIcoMin = 0.0;
        unsigned int mLoadingIcoMax = 0;

        /// Writes `mask` to the master camera and to the stereo pair, which are no-ops in mono.
        void cull(unsigned int mask);

        osg::ref_ptr<SceneUtil::SelectDepthFormatOperation> mSelectDepthFormatOperation;
        osg::ref_ptr<SceneUtil::Color::SelectColorFormatOperation> mSelectColorFormatOperation;

        std::unique_ptr<Stereo::Manager> mStereoManager;

        osg::ref_ptr<osgViewer::ScreenCaptureHandler> mScreenCaptureHandler;
        std::unique_ptr<ScreenshotManager> mScreenshotManager;

        /// The scene root this renderer made for the game, held from `createSceneRoot` until
        /// `attachWorld` hands it to the world that lights through it.
        osg::ref_ptr<SceneUtil::LightManager> mSceneRoot;

        /// Everything the rasterizer builds around the world, for as long as there is one.
        std::unique_ptr<GlWorld> mWorld;

        /// The last frame, copied off the framebuffer where it stands: upstream's loading-screen
        /// texture and its copy callback, made the first time the screen asks for them.
        osg::ref_ptr<osg::Texture2D> mFrozenFrame;
        osg::ref_ptr<CopyFramebufferToTextureCallback> mFreezeFrame;
        std::unique_ptr<MyGUIPlatform::OSGTexture> mFrozenFrameTexture;
    };
}

#endif
