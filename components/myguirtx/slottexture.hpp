#pragma once

#include <cstdint>
#include <string>

#include <MyGUI_IRenderTarget.h>
#include <MyGUI_ITexture.h>

#include <components/rtx/guirenderer.hpp>
#include <components/rtx/slot.hpp>

namespace osg
{
    class Image;
}

namespace MyGUIRtx
{
    /// A picture the GUI draws with, held as a slot in the renderer's own table: what the two
    /// kinds of texture here share, which is the slot, its size and its name. `Texture` is one the
    /// interface makes and writes; a `MirrorTexture` mirrors a picture the game holds.
    class SlotTexture : public MyGUI::ITexture
    {
    public:
        ~SlotTexture() override;

        SlotTexture(const SlotTexture&) = delete;
        SlotTexture& operator=(const SlotTexture&) = delete;

        const std::string& getName() const override { return mName; }

        int getWidth() const override { return mWidth; }
        int getHeight() const override { return mHeight; }

        void saveToFile(const std::string& fname) override;

        /// **Null, as it is in the other backend.** MyGUI can render a widget tree into a texture and
        /// neither of these has ever let it, so nothing in the game depends on it.
        MyGUI::IRenderTarget* getRenderTarget() override { return nullptr; }

        void setShader(const std::string& shaderName) override;

        /// Where this sits in the renderer's table, or nothing while it holds none.
        Rtx::GuiSlot getSlot() const { return mSlot; }

        /// Brings the slot up to date with whatever the texture is of, ahead of a draw: a mirror
        /// reads its image here, and a texture of the interface's own has nothing to read.
        virtual void refresh() {}

    protected:
        SlotTexture(std::string name, Rtx::GuiRenderer& renderer);

        /// A slot of this size, replacing whatever was held.
        void take(int width, int height);

        /// Takes the slot back and forgets the size, so the next `take` starts clean.
        void drop();

        /// The whole surface, which is what every write but a painted rectangle covers.
        Rtx::GuiRegion whole() const;

        /// Sends `image`, the slot's size, as the whole of it at four bytes a pixel. One `memcpy`
        /// where the image already is that, and a pixel at a time where it is not:
        /// `osg::Image::getColor` is the only thing that reads every format OpenSceneGraph loads,
        /// and it is a virtual call and a `Vec4f` per pixel.
        void sendImage(const osg::Image& image);

        Rtx::GuiRenderer& mRenderer;

    private:
        std::string mName;
        Rtx::GuiSlot mSlot;
        int mWidth = 0;
        int mHeight = 0;
    };
}
