#include "mirrortexture.hpp"

#include <stdexcept>

namespace MyGUIRtx
{
    void MirrorTexture::createManual(int, int, MyGUI::TextureUsage, MyGUI::PixelFormat)
    {
        throw std::logic_error("a mirrored picture is the game's, and is not made through MyGUI");
    }

    void MirrorTexture::loadFromFile(const std::string&)
    {
        throw std::logic_error("a mirrored picture is the game's, and is not loaded through MyGUI");
    }

    void* MirrorTexture::lock(MyGUI::TextureUsage)
    {
        throw std::logic_error("a mirrored picture is the game's, and is not written through MyGUI");
    }

    void MirrorTexture::unlock()
    {
        throw std::logic_error("a mirrored picture is the game's, and is not written through MyGUI");
    }
}
