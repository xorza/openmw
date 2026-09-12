#include "pixels.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

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
}
