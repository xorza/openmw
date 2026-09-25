#include "specularalbedo.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <osg/Vec3f>

#include "parallel.hpp"
#include "radicalinverse.hpp"
#include "shaders/brdf.h"

namespace Rtx
{
    namespace
    {
        constexpr std::uint32_t sSize = Shaders::SPECULAR_TABLE_SIZE;

        /// How many half vectors a node averages. Drawn from the normals the eye can see, so what is
        /// left of each term is `G2 / G1`, which is bounded by one: against the integral drawn with
        /// 2^20, the table is within 2.3e-4 at a cosine of 0.004 and 1.6e-4 elsewhere, where
        /// drawing from the distribution alone is eighteen times further off at that cosine.
        constexpr std::uint32_t sSamples = 4096;
    }

    const SpecularAlbedo& SpecularAlbedo::shared()
    {
        static const SpecularAlbedo table;
        return table;
    }

    SpecularAlbedo::SpecularAlbedo()
    {
        // One Hammersley set for every node: the azimuth at stratum centres, turned into its cosine
        // and sine once, and the cap's height off the radical inverse.
        std::vector<osg::Vec2f> turned(sSamples);
        std::vector<float> raised(sSamples);
        for (std::uint32_t at = 0; at < sSamples; ++at)
        {
            const float azimuth = Shaders::TAU * (static_cast<float>(at) + 0.5f) / static_cast<float>(sSamples);
            turned[at] = osg::Vec2f(std::cos(azimuth), std::sin(azimuth));
            raised[at] = radicalInverse(at, 2);
        }

        // A row a hand, each writing its own nodes and nothing else, so the table is the same
        // whichever hand took which row.
        mValues.resize(std::size_t{ sSize } * sSize * 2);
        runInParallel(
            sSize, [] { return 0; },
            [&](const std::size_t row) {
                const float alpha = Shaders::ggxAlpha(Shaders::specularTableRoughness(static_cast<std::uint32_t>(row)));

                for (std::uint32_t column = 0; column < sSize; ++column)
                {
                    const float cosine = Shaders::specularTableCosine(column);
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

                    const std::size_t node = (row * sSize + column) * 2;
                    mValues[node] = static_cast<float>(climbing / sSamples);
                    mValues[node + 1] = static_cast<float>(whole / sSamples);
                }
            });
    }

    osg::Vec2f SpecularAlbedo::at(float cosine, float roughness) const
    {
        const float across = Shaders::specularTableColumn(cosine);
        const float down = Shaders::specularTableRow(roughness);
        const auto left = static_cast<std::uint32_t>(across);
        const auto top = static_cast<std::uint32_t>(down);
        const std::uint32_t right = std::min(left + 1, sSize - 1);
        const std::uint32_t bottom = std::min(top + 1, sSize - 1);
        const float x = across - static_cast<float>(left);
        const float y = down - static_cast<float>(top);

        const auto node = [&](std::uint32_t column, std::uint32_t row) {
            const std::size_t at = (std::size_t{ row } * sSize + column) * 2;
            return osg::Vec2f(mValues[at], mValues[at + 1]);
        };

        return (node(left, top) * (1.0f - x) + node(right, top) * x) * (1.0f - y)
            + (node(left, bottom) * (1.0f - x) + node(right, bottom) * x) * y;
    }
}
