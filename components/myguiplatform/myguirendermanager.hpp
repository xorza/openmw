#ifndef OPENMW_COMPONENTS_MYGUIPLATFORM_MYGUIRENDERMANAGER_H
#define OPENMW_COMPONENTS_MYGUIPLATFORM_MYGUIRENDERMANAGER_H

#include <osg/ref_ptr>

#include "guirendermanager.hpp"

namespace Resource
{
    class ImageManager;
}

namespace Shader
{
    class ShaderManager;
}

namespace osg
{
    class Group;
    class Camera;
    class RenderInfo;
    class StateSet;
}

namespace MyGUIPlatform
{

    class Drawable;
    class OSGTexture;

    class RenderManager : public GuiRenderManager, public MyGUI::IRenderTarget
    {
        osg::ref_ptr<osg::Group> mSceneRoot;
        osg::ref_ptr<Drawable> mDrawable;
        Resource::ImageManager* mImageManager;

        /// The viewport the eye had when this was made. `initialise` is what applies it, and
        /// every resize after that arrives through `setViewSize`.
        MyGUI::IntSize mInitialViewSize;

        MyGUI::IntSize mViewSize;
        bool mUpdate;
        MyGUI::VertexColourType mVertexFormat;
        MyGUI::RenderTargetInfo mInfo;

        std::map<std::string, OSGTexture> mTextures;

        bool mIsInitialise;

        osg::ref_ptr<osg::Camera> mGuiRoot;

        float mInvScalingFactor;

        /// `SRC_ALPHA, ONE`, made once and handed to every batch while `setAdditiveBlend` is on.
        osg::ref_ptr<osg::StateSet> mAdditiveState;
        bool mAdditive = false;

    public:
        /// @param eye only for the viewport the GUI is first sized to. Every resize after that
        ///        arrives through `setViewSize`, so nothing here holds it.
        RenderManager(
            const osg::Camera& eye, osg::Group* sceneroot, Resource::ImageManager* imageManager, float scalingFactor);
        virtual ~RenderManager();

        void initialise() override;
        void shutdown() override;

        void enableShaders(Shader::ShaderManager& shaderManager);

        static RenderManager& getInstance() { return *getInstancePtr(); }
        static RenderManager* getInstancePtr()
        {
            return static_cast<RenderManager*>(MyGUI::RenderManager::getInstancePtr());
        }

        bool checkTexture(MyGUI::ITexture* texture) override;

        /** @see RenderManager::getViewSize */
        const MyGUI::IntSize& getViewSize() const override { return mViewSize; }

        /** @see RenderManager::getVertexFormat */
        MyGUI::VertexColourType getVertexFormat() const override { return mVertexFormat; }

        /** @see RenderManager::isFormatSupported */
        bool isFormatSupported(MyGUI::PixelFormat format, MyGUI::TextureUsage usage) override;

        /** @see RenderManager::createVertexBuffer */
        MyGUI::IVertexBuffer* createVertexBuffer() override;
        /** @see RenderManager::destroyVertexBuffer */
        void destroyVertexBuffer(MyGUI::IVertexBuffer* buffer) override;

        /** @see RenderManager::createTexture */
        MyGUI::ITexture* createTexture(const std::string& name) override;
        /** @see RenderManager::destroyTexture */
        void destroyTexture(MyGUI::ITexture* texture) override;
        /** @see RenderManager::getTexture */
        MyGUI::ITexture* getTexture(const std::string& name) override;

        // Called by the update traversal
        void update();

        // Called by the cull traversal
        /** @see IRenderTarget::begin */
        void begin() override;
        /** @see IRenderTarget::end */
        void end() override;
        /** @see IRenderTarget::doRender */
        void doRender(MyGUI::IVertexBuffer* buffer, MyGUI::ITexture* texture, size_t count) override;

        void setAdditiveBlend(bool additive) override;

        /** @see IRenderTarget::getInfo */
        const MyGUI::RenderTargetInfo& getInfo() const override { return mInfo; }

        void setViewSize(int width, int height) override;

        void registerShader(const std::string& shaderName, const std::string& vertexProgramFile,
            const std::string& fragmentProgramFile) override;

        /*internal:*/

        void collectDrawCalls();
    };

}

#endif
