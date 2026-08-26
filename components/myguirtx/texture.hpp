#ifndef OPENMW_COMPONENTS_MYGUIRTX_TEXTURE_H
#define OPENMW_COMPONENTS_MYGUIRTX_TEXTURE_H

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <MyGUI_ITexture.h>

#include <components/myguiplatform/regiontexture.hpp>
#include <components/rtx/renderer.hpp>

namespace Resource
{
    class ImageManager;
}

namespace MyGUIRtx
{

    /// A picture the GUI draws with, held as a slot in the renderer's own table.
    ///
    /// **The pixels live twice on purpose.** MyGUI's interface hands out a buffer to fill and takes
    /// it back filled, so there has to be one on this side; what goes to the device is a copy of it,
    /// made when the buffer comes back.
    class Texture final : public MyGUI::ITexture, public MyGUIPlatform::RegionTexture
    {
    public:
        Texture(std::string name, Rtx::Renderer& renderer, Resource::ImageManager* imageManager);
        ~Texture() override;

        Texture(const Texture&) = delete;
        Texture& operator=(const Texture&) = delete;

        const std::string& getName() const override { return mName; }

        void createManual(int width, int height, MyGUI::TextureUsage usage, MyGUI::PixelFormat format) override;
        void loadFromFile(const std::string& fname) override;
        void saveToFile(const std::string& fname) override;

        void destroy() override;

        void* lock(MyGUI::TextureUsage access) override;
        void unlock() override;
        bool isLocked() const override { return mLocked; }

        int getWidth() const override { return mWidth; }
        int getHeight() const override { return mHeight; }

        MyGUI::PixelFormat getFormat() const override { return mFormat; }
        MyGUI::TextureUsage getUsage() const override { return mUsage; }
        size_t getNumElemBytes() const override { return mNumElemBytes; }

        /// **Null, as it is in the other backend.** MyGUI can render a widget tree into a texture and
        /// neither of these has ever let it, so nothing in the game depends on it.
        MyGUI::IRenderTarget* getRenderTarget() override { return nullptr; }

        void setShader(const std::string& shaderName) override;

        /// **What MyGUI's own interface cannot ask for.** The pixels are kept on this side anyway,
        /// so writing part of them and sending that part is the whole of it — the world map paints
        /// eighteen pixels square when a cell arrives and used to send two megabytes.
        void writeRegion(std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height,
            std::span<const std::uint8_t> rows) override;

        /*internal:*/

        /// Where this sits in the renderer's table, or `sNoSlot` while it holds nothing.
        std::uint32_t getSlot() const { return mSlot; }

        static constexpr std::uint32_t sNoSlot = ~0u;

    private:
        /// Takes the slot back and forgets the size, so that a second `createManual` starts clean.
        void release();

        /// The whole surface, which is what every write but the world map's covers.
        Rtx::Renderer::GuiRegion whole() const;

        /// Sends the whole of `mPixels` to the renderer, widening it to four bytes a pixel where
        /// MyGUI asked for fewer. The scratch it widens into is kept, because a video frame comes
        /// through here once a frame.
        void upload();

        std::string mName;
        Rtx::Renderer& mRenderer;
        Resource::ImageManager* mImageManager;

        std::uint32_t mSlot = sNoSlot;
        int mWidth = 0;
        int mHeight = 0;
        MyGUI::PixelFormat mFormat = MyGUI::PixelFormat::Unknow;
        MyGUI::TextureUsage mUsage = MyGUI::TextureUsage::Default;
        std::size_t mNumElemBytes = 0;

        std::vector<std::uint8_t> mPixels;
        std::vector<std::uint8_t> mWidened;
        bool mLocked = false;
    };

}

#endif
