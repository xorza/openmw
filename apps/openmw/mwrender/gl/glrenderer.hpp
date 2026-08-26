#ifndef GAME_RENDER_GL_GLRENDERER_H
#define GAME_RENDER_GL_GLRENDERER_H

#include <memory>

#include <osg/ref_ptr>

#include "../renderer.hpp"

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

namespace SceneUtil
{
    class AsyncScreenCaptureOperation;
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

        int getMaxTextureUnits() const override { return mMaxTextureUnits; }

        bool wantsPagedTerrain() const override;
        float getTerrainCompositeMapLevel() const override;
        float getTerrainViewDistance(float cameraDistance, float fov) const override;
        SDL_Window* getWindow() const override { return mWindow; }

        void attachWorld(RenderingManager& world, osg::Group& worldRoot) override;
        void setSceneRoot(osg::Group& root) override;

        PostProcessor* getPostProcessor() override { return mPostProcessor.get(); }

        void advance(double simulationTime) override;
        void eventTraversal() override;
        void updateTraversal() override;
        void renderFrame(const SceneFrame& frame) override;

        std::unique_ptr<OffscreenView> createOffscreenView(const OffscreenViewSpec& spec) override;

        MyGUI::ITexture& freezeFrame() override;

        void renderGui() override;
        bool done() const override;

        void capture(osg::Image& image, int width, int height) override;
        void saveScreenshot() override;

        void suspendDraw() override;
        void resumeDraw() override;

        osgUtil::IncrementalCompileOperation* getCompileOperation() const override;
        void setCompileOperation(osgUtil::IncrementalCompileOperation* operation) override;

        void setVSync(SDLUtil::VSyncMode mode) override;

        void reloadChangedShaders(Shader::ShaderManager& shaders) override;

        std::unique_ptr<MyGUIPlatform::Platform> createGuiPlatform(osg::Group& guiRoot, Resource::ImageManager& images,
            Shader::ShaderManager& shaders, const VFS::Manager& vfs, float scalingFactor,
            VFS::Path::NormalizedView resourcePath, const std::filesystem::path& logPath) override;

        osg::Timer_t getStartTick() const override;

        void installStatsOverlay(const VFS::Manager& vfs, bool toFile) override;
        void reportStats(unsigned frameNumber, std::ostream& stream) const override;

    private:
        /// Makes the SDL window and the OpenGL context in it, retrying at half the antialiasing
        /// each time the driver refuses. Upstream's loop, unchanged.
        void createWindow(const std::filesystem::path& resourceDir);

        /// Takes the framebuffer copy back out of the frame once it has run. Left in, it would copy
        /// the whole screen into a texture on every frame from the first loading screen onwards.
        void retireFreezeFrame();

        Stage& mStage;
        int mMaxTextureUnits = 0;

        /// What an offscreen view's light rig is built out of. Known from `attachWorld` onwards,
        /// which is well before the GUI asks for the first view.
        Resource::ResourceSystem* mResources = nullptr;

        SDL_Window* mWindow = nullptr;

        /// Held so the destructor can let the GL context go while the window it is bound to still
        /// exists. The stage outlives the renderer and holds the camera, so releasing the viewer
        /// does not on its own release what the camera points at.
        osg::ref_ptr<SDLUtil::GraphicsWindowSDL2> mGraphicsWindow;

        osg::ref_ptr<osgViewer::Viewer> mViewer;

        osg::ref_ptr<SceneUtil::SelectDepthFormatOperation> mSelectDepthFormatOperation;
        osg::ref_ptr<SceneUtil::Color::SelectColorFormatOperation> mSelectColorFormatOperation;

        std::unique_ptr<Stereo::Manager> mStereoManager;

        osg::ref_ptr<SceneUtil::AsyncScreenCaptureOperation> mScreenCaptureOperation;
        osg::ref_ptr<osgViewer::ScreenCaptureHandler> mScreenCaptureHandler;
        std::unique_ptr<ScreenshotManager> mScreenshotManager;

        /// This renderer's frame graph. Everything between the scene and the screen.
        osg::ref_ptr<PostProcessor> mPostProcessor;

        /// The last frame, copied off the framebuffer where it stands. Made the first time the
        /// loading screen asks for one and re-armed every time after.
        osg::ref_ptr<osg::Texture2D> mFrozenFrame;
        osg::ref_ptr<CopyFramebufferToTextureCallback> mFreezeFrame;
        std::unique_ptr<MyGUIPlatform::OSGTexture> mFrozenFrameTexture;
        bool mFreezing = false;
    };
}

#endif
