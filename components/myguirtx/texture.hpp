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
    /// **The pixels are written once.** MyGUI's interface hands out a buffer to fill and takes it
    /// back filled, and the buffer handed out here is the renderer's own — the memory its copy to
    /// the device reads. A buffer of this class's own instead would put a crossing of main memory in
    /// front of every write, and a video frame arrives through here whole once a frame.
    ///
    /// **The exception is a format the table does not hold.** MyGUI asks for one, two or three
    /// channels as well as four, and those are widened on the way out — so they land in `mPixels`
    /// first, because the widening has to read them and the memory the renderer lends is written
    /// far faster than it is read.
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

        /// The whole surface, to be filled and handed back with `unlock`.
        ///
        /// **What comes back holds no picture.** `TextureUsage::Write` is a promise to fill the
        /// buffer, which every caller in this fork keeps: at four channels the bytes are the
        /// renderer's own and hold whatever it last copied out of them. Reading them is slow as well
        /// as wrong — see the class comment.
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

        /// **What MyGUI's own interface cannot ask for.** The world map paints eighteen pixels
        /// square when a cell arrives and used to send two megabytes.
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

        /// Widens `mPixels` into the renderer's own bytes and sends them, four channels out of
        /// however few MyGUI asked for.
        void widen();

        std::string mName;
        Rtx::Renderer& mRenderer;
        Resource::ImageManager* mImageManager;

        std::uint32_t mSlot = sNoSlot;
        int mWidth = 0;
        int mHeight = 0;
        MyGUI::PixelFormat mFormat = MyGUI::PixelFormat::Unknow;
        MyGUI::TextureUsage mUsage = MyGUI::TextureUsage::Default;
        std::size_t mNumElemBytes = 0;

        /// What MyGUI fills at fewer than four channels, and empty at four.
        std::vector<std::uint8_t> mPixels;
        bool mLocked = false;
    };

}

#endif
