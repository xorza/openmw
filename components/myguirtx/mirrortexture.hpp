#pragma once

#include <cstddef>
#include <string>

#include <MyGUI_RenderFormat.h>

#include "slottexture.hpp"

namespace MyGUIRtx
{
    /// A mirror of a picture the game holds in main memory, which a widget draws and nothing
    /// writes through MyGUI: the picture is the game's, so a call to make, load or lock it here is
    /// a caller that misunderstood what it holds, and throws. What each mirror owes is `refresh`:
    /// how it finds what the game wrote since the last draw.
    class MirrorTexture : public SlotTexture
    {
    public:
        void createManual(int width, int height, MyGUI::TextureUsage usage, MyGUI::PixelFormat format) override;
        void loadFromFile(const std::string& fname) override;
        void destroy() override { drop(); }

        void* lock(MyGUI::TextureUsage access) override;
        void unlock() override;
        bool isLocked() const override { return false; }

        MyGUI::PixelFormat getFormat() const override { return MyGUI::PixelFormat::R8G8B8A8; }
        MyGUI::TextureUsage getUsage() const override { return MyGUI::TextureUsage::Static; }
        size_t getNumElemBytes() const override { return 4; }

    protected:
        explicit MirrorTexture(Rtx::GuiRenderer& renderer)
            : SlotTexture({}, renderer)
        {
        }
    };
}
