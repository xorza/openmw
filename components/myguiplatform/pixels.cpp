#include "pixels.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>

#include <osg/Image>

namespace MyGUIPlatform
{
    namespace
    {
        /// One texel of `area` in `image`, filtered `GL_LINEAR` with `GL_CLAMP_TO_EDGE`, into four
        /// bytes at `out`.
        ///
        /// **The one filter both public shapes are made of.** A sampler's answer and a whole
        /// rectangle of them are the same arithmetic asked once or asked in a loop, and two spellings
        /// of it are two roundings waiting to disagree about a map the game has always drawn one way.
        ///
        /// `u` and `v` are in texels of `area`, measured from its corner and already offset by the
        /// half texel a sampler puts between a coordinate and a centre. The clamp is to `area` and
        /// not to the image around it, so a rectangle filters as though it were the whole picture.
        void filterTexel(const osg::Image& image, const Rect& area, float u, float v, std::uint8_t* out)
        {
            const float flooredU = std::floor(u);
            const float flooredV = std::floor(v);
            const float fracU = u - flooredU;
            const float fracV = v - flooredV;

            const auto clamped = [](float at, int extent) { return std::clamp(static_cast<int>(at), 0, extent - 1); };

            const int left = area.mX + clamped(flooredU, area.mWidth);
            const int right = area.mX + clamped(flooredU + 1.0f, area.mWidth);
            const int bottom = area.mY + clamped(flooredV, area.mHeight);
            const int top = area.mY + clamped(flooredV + 1.0f, area.mHeight);

            const std::uint8_t* lowerLeft = image.data(left, bottom);
            const std::uint8_t* lowerRight = image.data(right, bottom);
            const std::uint8_t* upperLeft = image.data(left, top);
            const std::uint8_t* upperRight = image.data(right, top);

            for (int channel = 0; channel < 4; ++channel)
            {
                const float lower
                    = std::lerp(static_cast<float>(lowerLeft[channel]), static_cast<float>(lowerRight[channel]), fracU);
                const float upper
                    = std::lerp(static_cast<float>(upperLeft[channel]), static_cast<float>(upperRight[channel]), fracU);

                out[channel] = static_cast<std::uint8_t>(std::lround(std::lerp(lower, upper, fracV)));
            }
        }
    }

    std::optional<MyGUI::PixelFormat> directFormat(const osg::Image& image)
    {
        if (image.getDataType() != GL_UNSIGNED_BYTE || !image.isDataContiguous())
            return {};

        switch (image.getPixelFormat())
        {
            case GL_LUMINANCE:
                return MyGUI::PixelFormat::L8;
            case GL_LUMINANCE_ALPHA:
                return MyGUI::PixelFormat::L8A8;
            case GL_RGB:
                return MyGUI::PixelFormat::R8G8B8;
            case GL_RGBA:
                return MyGUI::PixelFormat::R8G8B8A8;
            default:
                return {};
        }
    }

    std::size_t bytesPerPixel(MyGUI::PixelFormat format)
    {
        switch (format.getValue())
        {
            case MyGUI::PixelFormat::L8:
                return 1;
            case MyGUI::PixelFormat::L8A8:
                return 2;
            case MyGUI::PixelFormat::R8G8B8:
                return 3;
            default:
                return 4;
        }
    }

    void writeRgba(const osg::Image& image, std::uint8_t* into)
    {
        const std::size_t count = static_cast<std::size_t>(image.s()) * image.t();

        if (directFormat(image) == MyGUI::PixelFormat::R8G8B8A8 && image.getTotalSizeInBytes() == count * 4)
        {
            std::memcpy(into, image.data(), count * 4);
            return;
        }

        for (int y = 0; y < image.t(); ++y)
            for (int x = 0; x < image.s(); ++x, into += 4)
            {
                const osg::Vec4f colour = image.getColor(x, y);
                into[0] = static_cast<std::uint8_t>(std::clamp(colour.r(), 0.f, 1.f) * 255.f + 0.5f);
                into[1] = static_cast<std::uint8_t>(std::clamp(colour.g(), 0.f, 1.f) * 255.f + 0.5f);
                into[2] = static_cast<std::uint8_t>(std::clamp(colour.b(), 0.f, 1.f) * 255.f + 0.5f);
                into[3] = static_cast<std::uint8_t>(std::clamp(colour.a(), 0.f, 1.f) * 255.f + 0.5f);
            }
    }

    void sampleBilinear(const osg::Image& image, float u, float v, std::uint8_t (&out)[4])
    {
        filterTexel(image, Rect{ 0, 0, image.s(), image.t() }, u * static_cast<float>(image.s()) - 0.5f,
            v * static_cast<float>(image.t()) - 0.5f, out);
    }

    void resampleRegion(const osg::Image& from, const Rect& source, osg::Image& into, const Rect& target)
    {
        assert(source.mWidth > 0 && source.mHeight > 0 && target.mWidth > 0 && target.mHeight > 0);

        const float acrossU = static_cast<float>(source.mWidth) / static_cast<float>(target.mWidth);
        const float acrossV = static_cast<float>(source.mHeight) / static_cast<float>(target.mHeight);

        for (int y = 0; y < target.mHeight; ++y)
        {
            const float v = (static_cast<float>(y) + 0.5f) * acrossV - 0.5f;

            for (int x = 0; x < target.mWidth; ++x)
            {
                const float u = (static_cast<float>(x) + 0.5f) * acrossU - 0.5f;

                filterTexel(from, source, u, v, into.data(target.mX + x, target.mY + y));
            }
        }
    }

    void gatherRegion(const osg::Image& image, const Rect& area, std::vector<std::uint8_t>& rows)
    {
        assert(image.isDataContiguous());
        assert(area.mX >= 0 && area.mY >= 0 && area.mWidth >= 0 && area.mHeight >= 0);
        assert(area.mX + area.mWidth <= image.s() && area.mY + area.mHeight <= image.t());

        rows.resize(static_cast<std::size_t>(area.mWidth) * area.mHeight * 4);

        for (int row = 0; row < area.mHeight; ++row)
            std::memcpy(rows.data() + static_cast<std::size_t>(row) * area.mWidth * 4,
                image.data(area.mX, area.mY + row), static_cast<std::size_t>(area.mWidth) * 4);
    }
}
