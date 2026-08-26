#include "rendermanager.hpp"

#include <algorithm>
#include <cstring>

#include <MyGUI_Timer.h>
#include <MyGUI_VertexData.h>

#include <components/debug/debuglog.hpp>

#include "texture.hpp"

namespace MyGUIRtx
{
    namespace
    {
        static_assert(sizeof(MyGUI::Vertex) == sizeof(Rtx::GuiVertex),
            "the renderer reads MyGUI's own vertices, so the two have to be the same twenty-four bytes");

        /// MyGUI's vertices for one buffer, kept until the frame that draws them takes a copy.
        ///
        /// **No double buffering, unlike the other backend's.** That one hands its arrays to a draw
        /// thread that runs a frame behind; here the whole frame is gathered, copied and submitted
        /// between `collectDrawCalls` and its return, so there is never a second reader.
        class VertexBuffer final : public MyGUI::IVertexBuffer
        {
        public:
            void setVertexCount(size_t count) override { mVertices.resize(count); }
            size_t getVertexCount() const override { return mVertices.size(); }

            MyGUI::Vertex* lock() override { return mVertices.data(); }
            void unlock() override {}

            std::span<const MyGUI::Vertex> get(size_t count) const
            {
                return std::span(mVertices).first(std::min(count, mVertices.size()));
            }

        private:
            std::vector<MyGUI::Vertex> mVertices;
        };
    }

    RenderManager::RenderManager(Rtx::Renderer& renderer, Resource::ImageManager* imageManager, float scalingFactor)
        : mRenderer(renderer)
        , mImageManager(imageManager)
    {
        if (scalingFactor != 0.f)
            mInvScalingFactor = 1.f / scalingFactor;
    }

    RenderManager::~RenderManager()
    {
        mIsInitialise = false;
    }

    void RenderManager::initialise()
    {
        MYGUI_ASSERT(!mIsInitialise, getClassTypeName() << " initialised twice");

        mVertexFormat = MyGUI::VertexColourType::ColourABGR;
        mUpdate = false;

        const Rtx::FrameExtents extents = mRenderer.getExtents();
        setViewSize(static_cast<int>(extents.mOutputWidth), static_cast<int>(extents.mOutputHeight));

        mIsInitialise = true;
    }

    void RenderManager::shutdown()
    {
        // Nothing is attached to anything: the renderer is asked to draw and there is no graph to
        // take this back out of.
    }

    void RenderManager::setAdditiveBlend(bool additive)
    {
        mBlend = additive ? Rtx::GuiBlend::Additive : Rtx::GuiBlend::Over;
    }

    bool RenderManager::checkTexture(MyGUI::ITexture* /*texture*/)
    {
        // We support external textures that aren't registered via this manager, so can't implement
        // this method sensibly. The other backend answers the same way, and for the same reason.
        return true;
    }

    bool RenderManager::isFormatSupported(MyGUI::PixelFormat /*format*/, MyGUI::TextureUsage /*usage*/)
    {
        return true;
    }

    MyGUI::IVertexBuffer* RenderManager::createVertexBuffer()
    {
        return new VertexBuffer();
    }

    void RenderManager::destroyVertexBuffer(MyGUI::IVertexBuffer* buffer)
    {
        delete buffer;
    }

    MyGUI::ITexture* RenderManager::createTexture(const std::string& name)
    {
        const auto it = mTextures.find(name);
        if (it != mTextures.end())
        {
            // **Emptied in place rather than replaced.** MyGUI hands these pointers to widgets and
            // asks for a texture under a name it already holds — a map tile remade, a save
            // thumbnail, a font rebuilt on a resize — and a widget still showing the old one must
            // not be left pointing at freed memory. The other backend keeps the address by holding
            // its textures by value; this one has to be told to.
            it->second->destroy();
            return it->second.get();
        }

        return mTextures.emplace(name, std::make_unique<Texture>(name, mRenderer, mImageManager)).first->second.get();
    }

    void RenderManager::destroyTexture(MyGUI::ITexture* texture)
    {
        if (texture == nullptr)
            return;

        const auto it = mTextures.find(texture->getName());
        MYGUI_ASSERT(it != mTextures.end(), "Texture '" << texture->getName() << "' not found");

        mTextures.erase(it);
    }

    MyGUI::ITexture* RenderManager::getTexture(const std::string& name)
    {
        if (name.empty())
            return nullptr;

        const auto it = mTextures.find(name);
        if (it == mTextures.end())
        {
            // **Loaded rather than looked up in MyGUI's data manager.** A name here is a path into
            // the game's virtual file system, which MyGUI knows nothing about; the image manager
            // resolves it and answers a missing file with a placeholder rather than a failure.
            MyGUI::ITexture* texture = createTexture(name);
            texture->loadFromFile(name);
            return texture;
        }

        return it->second.get();
    }

    void RenderManager::begin()
    {
        mVertices.clear();
        mBatches.clear();
    }

    void RenderManager::doRender(MyGUI::IVertexBuffer* buffer, MyGUI::ITexture* texture, size_t count)
    {
        const auto* texel = static_cast<const Texture*>(texture);
        if (texel == nullptr || texel->getSlot() == Texture::sNoSlot || count == 0)
            return;

        const std::span<const MyGUI::Vertex> vertices = static_cast<const VertexBuffer*>(buffer)->get(count);
        if (vertices.empty())
            return;

        const std::uint32_t first = static_cast<std::uint32_t>(mVertices.size());
        mVertices.resize(mVertices.size() + vertices.size());
        std::memcpy(mVertices.data() + first, vertices.data(), vertices.size_bytes());

        mBatches.push_back(Rtx::GuiBatch{
            .mTexture = texel->getSlot(),
            .mFirstVertex = first,
            .mVertexCount = static_cast<std::uint32_t>(vertices.size()),
            .mBlend = mBlend,
        });
    }

    void RenderManager::end() {}

    void RenderManager::update()
    {
        static MyGUI::Timer timer;
        static unsigned long lastTime = timer.getMilliseconds();
        const unsigned long nowTime = timer.getMilliseconds();
        const unsigned long time = nowTime - lastTime;

        onFrameEvent(static_cast<float>(static_cast<double>(time) / 1000));

        lastTime = nowTime;
    }

    void RenderManager::collectDrawCalls()
    {
        if (!mIsInitialise)
            return;

        begin();
        onRenderToTarget(this, mUpdate);
        end();

        mUpdate = false;

        mRenderer.drawGui(mVertices, mBatches);
    }

    void RenderManager::setViewSize(int width, int height)
    {
        if (width < 1)
            width = 1;
        if (height < 1)
            height = 1;

        mViewSize.set(static_cast<int>(width * mInvScalingFactor), static_cast<int>(height * mInvScalingFactor));

        mInfo.maximumDepth = 1;
        mInfo.hOffset = 0;
        mInfo.vOffset = 0;
        mInfo.aspectCoef = float(mViewSize.height) / float(mViewSize.width);
        mInfo.pixScaleX = 1.0f / float(mViewSize.width);
        mInfo.pixScaleY = 1.0f / float(mViewSize.height);

        onResizeView(mViewSize);
        mUpdate = true;
    }

    void RenderManager::registerShader(const std::string& shaderName, const std::string& /*vertexProgramFile*/,
        const std::string& /*fragmentProgramFile*/)
    {
        // The other backend compiles one and draws widgets with it; this one has two pipelines
        // built at startup and no way to take a third.
        Log(Debug::Warning) << "A GUI shader was asked for and this backend has none: " << shaderName;
    }
}
