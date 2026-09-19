#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <MyGUI_IRenderTarget.h>
#include <MyGUI_RenderFormat.h>
#include <MyGUI_RenderManager.h>
#include <MyGUI_RenderTargetInfo.h>
#include <MyGUI_Types.h>

#include <components/myguiplatform/guirendermanager.hpp>
#include <components/rtx/guirenderer.hpp>

namespace Resource
{
    class ImageManager;
}

namespace MyGUIRtx
{

    class RenderManager;
    class Texture;

    /// What ends a texture a caller took under its own name: the manager's own destroy, which is
    /// the one MyGUI's widgets would call.
    struct TextureDestroyer
    {
        RenderManager* mManager = nullptr;

        void operator()(Texture* texture) const;
    };

    /// A texture the caller owns for as long as the handle stands, and the manager files under its
    /// name for as long as MyGUI needs to find it. What a view inside the interface holds instead
    /// of a reference it has to remember to destroy: a constructor that throws after the texture
    /// was made unwinds the handle, and the slot the texture took goes with it.
    using TextureHandle = std::unique_ptr<Texture, TextureDestroyer>;

    /// MyGUI over `Rtx::GuiRenderer`, whichever graphics API is behind that.
    ///
    /// **Written once for every backend, and that is the whole design.** MyGUI's own interface is
    /// thirty functions and most of them are bookkeeping no API has an opinion about: a name-to-
    /// texture map, a view size, a lock and unlock contract, the batching. What is left is a table
    /// of textures and one call that draws a list of triangles, and those are what `Rtx::GuiRenderer`
    /// offers. The scene, the frame and the instruments are three interfaces this cannot reach.
    ///
    /// **Nothing here is driven by a scene graph.** The other backend hangs its update on an OSG
    /// update callback and its draw on a cull callback; this one is called by the renderer's frame
    /// directly, which is why a renderer that traverses but never culls still draws a GUI.
    class RenderManager final : public MyGUIPlatform::GuiRenderManager, public MyGUI::IRenderTarget
    {
    public:
        /// @param scalingFactor how many device pixels a GUI pixel is worth. Zero means one.
        RenderManager(Rtx::GuiRenderer& renderer, Resource::ImageManager* imageManager, float scalingFactor);
        ~RenderManager() override;

        RenderManager(const RenderManager&) = delete;
        RenderManager& operator=(const RenderManager&) = delete;

        void initialise() override;
        void shutdown() override;

        /// This backend's `AdditiveLayer`, under the type name upstream's has.
        void registerFactories() override;

        /// Whether what is drawn from now on is added to what is under it rather than blended over
        /// it. `AdditiveLayer` turns it on around the one layer that wants it and off again.
        void setAdditiveBlend(bool additive);

        /// A mirror of the texture's image, read again whenever the game marks it dirty — `Texture`
        /// says how. An `osg::Texture2D` itself is a name in a context this backend never made.
        std::unique_ptr<MyGUI::ITexture> shareTexture(osg::Texture2D& texture) override;

        /// A mirror told where its picture was painted, which sends that and no more.
        std::unique_ptr<MyGUI::ITexture> shareTexture(SceneUtil::PaintedTexture& texture) override;

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

        /// `createTexture`, as the type it makes: for a view inside the interface, which traces
        /// into the slot the texture holds and so needs to ask for it.
        Texture& makeTexture(const std::string& name);

        /// `makeTexture` under a name nothing holds yet, owned by the caller: the handle destroys
        /// it here when the caller lets go. The name is asserted new, because a handle over a
        /// texture MyGUI already hands to widgets would be a second owner.
        TextureHandle takeTexture(const std::string& name);

        void destroyTexture(MyGUI::ITexture* texture) override;
        MyGUI::ITexture* getTexture(const std::string& name) override;

        void begin() override;
        void end() override;
        void doRender(MyGUI::IVertexBuffer* buffer, MyGUI::ITexture* texture, size_t count) override;

        const MyGUI::RenderTargetInfo& getInfo() const override { return mInfo; }

        void setViewSize(int width, int height) override;

        void registerShader(const std::string& shaderName, const std::string& vertexProgramFile,
            const std::string& fragmentProgramFile) override;

        /// One frame of widget animation, `step` seconds of it.
        ///
        /// **The step is the caller's, because the interface ages with everything else.** MyGUI's
        /// own timer is a wall clock read in whole milliseconds, and every animation hanging off
        /// this call ages by it — the screen faders included, which are the red overlay a hit puts
        /// up and the black one a fade uses. A run that steps its world by the frame index and its
        /// interface by the wall draws that overlay at a different strength on the same frame in
        /// every run. `Misc::FrameClock` is what a caller answers from.
        void update(float step);

        /// Gathers every layer's triangles and hands them to the renderer in one call.
        void collectDrawCalls();

    private:
        Rtx::GuiRenderer& mRenderer;
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
