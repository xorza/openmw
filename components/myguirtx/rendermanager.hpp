#ifndef OPENMW_COMPONENTS_MYGUIRTX_RENDERMANAGER_H
#define OPENMW_COMPONENTS_MYGUIRTX_RENDERMANAGER_H

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <MyGUI_IRenderTarget.h>

#include <components/myguiplatform/guirendermanager.hpp>
#include <components/rtx/renderer.hpp>

namespace Resource
{
    class ImageManager;
}

namespace MyGUIRtx
{

    class Texture;

    /// MyGUI over `Rtx::Renderer`, whichever graphics API is behind that.
    ///
    /// **Written once for every backend, and that is the whole design.** MyGUI's own interface is
    /// thirty functions and most of them are bookkeeping no API has an opinion about: a name-to-
    /// texture map, a view size, a lock and unlock contract, the batching. What is left is a table
    /// of textures and one call that draws a list of triangles, and those are the four things
    /// `Rtx::Renderer` offers.
    ///
    /// **Nothing here is driven by a scene graph.** The other backend hangs its update on an OSG
    /// update callback and its draw on a cull callback; this one is called by the renderer's frame
    /// directly, which is why a renderer that traverses but never culls still draws a GUI.
    class RenderManager final : public MyGUIPlatform::GuiRenderManager, public MyGUI::IRenderTarget
    {
    public:
        /// @param scalingFactor how many device pixels a GUI pixel is worth. Zero means one.
        RenderManager(Rtx::Renderer& renderer, Resource::ImageManager* imageManager, float scalingFactor);
        ~RenderManager() override;

        RenderManager(const RenderManager&) = delete;
        RenderManager& operator=(const RenderManager&) = delete;

        void initialise() override;
        void shutdown() override;
        void setAdditiveBlend(bool additive) override;

        static RenderManager& getInstance() { return *getInstancePtr(); }
        static RenderManager* getInstancePtr()
        {
            return static_cast<RenderManager*>(MyGUI::RenderManager::getInstancePtr());
        }

        bool checkTexture(MyGUI::ITexture* texture) override;

        const MyGUI::IntSize& getViewSize() const override { return mViewSize; }
        MyGUI::VertexColourType getVertexFormat() const override { return mVertexFormat; }
        bool isFormatSupported(MyGUI::PixelFormat format, MyGUI::TextureUsage usage) override;

        MyGUI::IVertexBuffer* createVertexBuffer() override;
        void destroyVertexBuffer(MyGUI::IVertexBuffer* buffer) override;

        MyGUI::ITexture* createTexture(const std::string& name) override;
        void destroyTexture(MyGUI::ITexture* texture) override;
        MyGUI::ITexture* getTexture(const std::string& name) override;

        void begin() override;
        void end() override;
        void doRender(MyGUI::IVertexBuffer* buffer, MyGUI::ITexture* texture, size_t count) override;

        const MyGUI::RenderTargetInfo& getInfo() const override { return mInfo; }

        void setViewSize(int width, int height) override;

        void registerShader(const std::string& shaderName, const std::string& vertexProgramFile,
            const std::string& fragmentProgramFile) override;

        /*internal:*/

        /// One frame of widget animation. The other backend gets this from an update callback.
        void update();

        /// Gathers every layer's triangles and hands them to the renderer in one call.
        void collectDrawCalls();

    private:
        Rtx::Renderer& mRenderer;
        Resource::ImageManager* mImageManager;

        MyGUI::IntSize mViewSize;
        MyGUI::VertexColourType mVertexFormat = MyGUI::VertexColourType::ColourABGR;
        MyGUI::RenderTargetInfo mInfo{};

        std::map<std::string, std::unique_ptr<Texture>> mTextures;

        /// Every batch's vertices, gathered into one buffer as the layers hand them over, and the
        /// runs that say which texture each stretch of it belongs to. Both are cleared and refilled
        /// every frame rather than rebuilt, because this happens once a frame forever.
        std::vector<Rtx::GuiVertex> mVertices;
        std::vector<Rtx::GuiBatch> mBatches;

        float mInvScalingFactor = 1.0f;

        /// What every batch gathered from here on is marked with. `AdditiveLayer` moves it either
        /// way around the one layer that wants it.
        Rtx::GuiBlend mBlend = Rtx::GuiBlend::Over;

        bool mUpdate = false;
        bool mIsInitialise = false;
    };

}

#endif
