#include "picture.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>

#include <MyGUI_ITexture.h>
#include <MyGUI_RenderManager.h>

#include <osg/Image>

#include "pixels.hpp"
#include "regiontexture.hpp"

namespace MyGUIPlatform
{

    Picture::Picture(std::string_view label)
    {
        static unsigned int next = 0;
        mName = std::string(label) + " " + std::to_string(next++);
    }

    Picture::~Picture()
    {
        release();
    }

    void Picture::release()
    {
        if (mTexture == nullptr)
            return;

        // **Asked for rather than assumed, because a picture can outlive the interface.** The
        // renderer holds one for the frozen frame and `Engine`'s destructor lets go of the window
        // manager a dozen lines before the renderer — so by the time this runs there is no table to
        // give a texture back to. MyGUI's own accessor asserts in that case, and its assert logs,
        // and logging asks for the log manager that went with it, until the stack runs out.
        if (MyGUI::RenderManager* manager = MyGUI::RenderManager::getInstancePtr())
            manager->destroyTexture(mTexture);

        mTexture = nullptr;
        mRegion = nullptr;
    }

    Picture::Picture(Picture&& other) noexcept
        : mRegionScratch(std::move(other.mRegionScratch))
        , mName(std::move(other.mName))
        , mTexture(std::exchange(other.mTexture, nullptr))
        , mRegion(std::exchange(other.mRegion, nullptr))
        , mWidth(other.mWidth)
        , mHeight(other.mHeight)
        , mFormat(other.mFormat)
    {
    }

    Picture& Picture::operator=(Picture&& other) noexcept
    {
        if (this == &other)
            return *this;

        release();

        mRegionScratch = std::move(other.mRegionScratch);
        mName = std::move(other.mName);
        mTexture = std::exchange(other.mTexture, nullptr);
        mRegion = std::exchange(other.mRegion, nullptr);
        mWidth = other.mWidth;
        mHeight = other.mHeight;
        mFormat = other.mFormat;

        return *this;
    }

    void Picture::setRegion(const osg::Image& image, int x, int y, int width, int height)
    {
        // **The whole thing where the backend has nothing better**, which is exactly what a caller
        // would otherwise have written for itself. `mFormat` is what says whether the rows below
        // would even be the right bytes: a picture the GUI took at three channels is widened on the
        // way through `set` and has no packed rectangle to send.
        if (mRegion == nullptr || mFormat != MyGUI::PixelFormat::R8G8B8A8)
        {
            set(image);
            return;
        }

        assert(x >= 0 && y >= 0 && x + width <= mWidth && y + height <= mHeight);

        gatherRegion(image, x, y, width, height, mRegionScratch);
        mRegion->writeRegion(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
            static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), mRegionScratch);
    }

    void Picture::set(const osg::Image& image)
    {
        const std::optional<MyGUI::PixelFormat> direct = directFormat(image);
        const bool wholeRows = direct.has_value()
            && image.getTotalSizeInBytes() == static_cast<std::size_t>(image.s()) * image.t() * bytesPerPixel(*direct);
        const MyGUI::PixelFormat format = wholeRows ? *direct : MyGUI::PixelFormat::R8G8B8A8;

        if (mTexture == nullptr || mWidth != image.s() || mHeight != image.t() || mFormat != format)
        {
            // MyGUI keys its textures by name and remaking one under a name it already knows
            // replaces it in place, so anything already showing this picture keeps pointing at it.
            mTexture = MyGUI::RenderManager::getInstance().createTexture(mName);
            mTexture->createManual(
                image.s(), image.t(), MyGUI::TextureUsage::Static | MyGUI::TextureUsage::Write, format);

            mRegion = dynamic_cast<RegionTexture*>(mTexture);

            mWidth = image.s();
            mHeight = image.t();
            mFormat = format;
        }

        auto* pixels = static_cast<std::uint8_t*>(mTexture->lock(MyGUI::TextureUsage::Write));

        if (wholeRows)
            std::memcpy(pixels, image.data(), image.getTotalSizeInBytes());
        else
            writeRgba(image, pixels);

        mTexture->unlock();
    }

}
