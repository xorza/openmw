#include "specularalbedo.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <osg/Vec3f>

#include "radicalinverse.hpp"
#include "shaders/brdf.h"

namespace Rtx
{
    namespace
    {
        constexpr std::uint32_t sSize = Shaders::SPECULAR_TABLE_SIZE;

        /// How many half vectors a cell averages. Drawn from the normals the eye can see, so what is
        /// left of each term is `G2 / G1`, which is bounded by one: the test's quadrature finds the
        /// table within about a part in ten thousand, grazing included, where drawing from the
        /// distribution alone was fifty times further off at this count.
        constexpr std::uint32_t sSamples = 4096;

    }

    const SpecularAlbedo& SpecularAlbedo::shared()
    {
        static const SpecularAlbedo table;
        return table;
    }

    SpecularAlbedo::SpecularAlbedo()
    {
        // One Hammersley set for every cell: the azimuth at stratum centres, turned into its cosine
        // and sine once, and the cap's height off the radical inverse.
        std::vector<osg::Vec2f> turned(sSamples);
        std::vector<float> raised(sSamples);
        for (std::uint32_t at = 0; at < sSamples; ++at)
        {
            const float azimuth = Shaders::TAU * (static_cast<float>(at) + 0.5f) / static_cast<float>(sSamples);
            turned[at] = osg::Vec2f(std::cos(azimuth), std::sin(azimuth));
            raised[at] = radicalInverse(at, 2);
        }

        mValues.resize(std::size_t{ sSize } * sSize * 2);
        for (std::uint32_t row = 0; row < sSize; ++row)
        {
            const float roughness = (static_cast<float>(row) + 0.5f) / static_cast<float>(sSize);
            const float alpha = Shaders::ggxAlpha(roughness);

            for (std::uint32_t column = 0; column < sSize; ++column)
            {
                const float cosine = (static_cast<float>(column) + 0.5f) / static_cast<float>(sSize);
                const osg::Vec3f eye(std::sqrt(1.0f - cosine * cosine), 0.0f, cosine);

                double climbing = 0.0;
                double whole = 0.0;
                for (std::uint32_t at = 0; at < sSamples; ++at)
                {
                    const osg::Vec3f half = Shaders::visibleNormal(eye, alpha, raised[at], turned[at]);

                    const float eyeHalf = eye * half;
                    const float lightCosine = 2.0f * eyeHalf * half.z() - cosine;
                    if (!(eyeHalf > 0.0f) || !(lightCosine > 0.0f))
                        continue;

                    const float weight = Shaders::smithShadowingGivenMasking(alpha, cosine, lightCosine);
                    climbing += static_cast<double>(weight * Shaders::schlickWeight(eyeHalf));
                    whole += static_cast<double>(weight);
                }

                const std::size_t cell = (std::size_t{ row } * sSize + column) * 2;
                mValues[cell] = static_cast<float>(climbing / sSamples);
                mValues[cell + 1] = static_cast<float>(whole / sSamples);
            }
        }
    }

    osg::Vec2f SpecularAlbedo::at(float cosine, float roughness) const
    {
        const float last = static_cast<float>(sSize - 1);
        const float across = std::clamp(cosine * static_cast<float>(sSize) - 0.5f, 0.0f, last);
        const float down = std::clamp(roughness * static_cast<float>(sSize) - 0.5f, 0.0f, last);
        const auto left = static_cast<std::uint32_t>(across);
        const auto top = static_cast<std::uint32_t>(down);
        const std::uint32_t right = std::min(left + 1, sSize - 1);
        const std::uint32_t bottom = std::min(top + 1, sSize - 1);
        const float x = across - static_cast<float>(left);
        const float y = down - static_cast<float>(top);

        const auto cell = [&](std::uint32_t column, std::uint32_t row) {
            const std::size_t at = (std::size_t{ row } * sSize + column) * 2;
            return osg::Vec2f(mValues[at], mValues[at + 1]);
        };

        return (cell(left, top) * (1.0f - x) + cell(right, top) * x) * (1.0f - y)
            + (cell(left, bottom) * (1.0f - x) + cell(right, bottom) * x) * y;
    }
}
