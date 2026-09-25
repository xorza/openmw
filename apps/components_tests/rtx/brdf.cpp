#include <cmath>
#include <cstdint>

#include <gtest/gtest.h>

#include <osg/Vec2f>

#include <components/rtx/shaders/brdf.h>
#include <components/rtx/specularalbedo.hpp>

namespace Rtx
{
    namespace
    {
        /// **A vanilla surface is the Lambert surface it was.** A reflectance of nought has an edge of
        /// nought, reflects exactly nothing at every angle, has a directional albedo of exactly
        /// nothing wherever the table is read, and is scaled by exactly one. A dielectric's 4% reaches
        /// an edge of one: `50 * 0.04` is two, saturated.
        TEST(RtxBrdfTest, aReflectanceOfNoughtReflectsExactlyNothing)
        {
            const float edge = Shaders::specularEdge(0.0f);
            EXPECT_EQ(edge, 0.0f);
            EXPECT_EQ(Shaders::specularEdge(Shaders::DIELECTRIC_F0), 1.0f);

            for (const float weight : { 0.0f, 0.25f, 1.0f })
                EXPECT_EQ(Shaders::fresnelSchlick(0.0f, edge, weight), 0.0f) << weight;

            const SpecularAlbedo& table = SpecularAlbedo::shared();
            for (const float roughness : { 0.0f, 0.3f, 1.0f })
                for (const float cosine : { 0.0f, 0.5f, 1.0f })
                {
                    const osg::Vec2f cell = table.at(cosine, roughness);
                    EXPECT_EQ(Shaders::specularAlbedoOf(0.0f, edge, cell.x(), cell.y()), 0.0f)
                        << cosine << ' ' << roughness;
                    EXPECT_EQ(Shaders::specularCompensation(0.0f, cell.y()), 1.0f) << cosine << ' ' << roughness;
                }
        }

        /// **The lobe is reciprocal**: its visibility term is the same with the eye and the light
        /// swapped, to the rounding of a sum taken in the other order.
        TEST(RtxBrdfTest, theLobeIsReciprocal)
        {
            for (const float alpha : { 0.002f, 0.1f, 0.5f, 1.0f })
                for (const float toEye : { 0.05f, 0.3f, 0.7f, 1.0f })
                    for (const float toLight : { 0.02f, 0.4f, 0.9f })
                        EXPECT_FLOAT_EQ(Shaders::smithVisibility(alpha, toEye, toLight),
                            Shaders::smithVisibility(alpha, toLight, toEye))
                            << alpha << ' ' << toEye << ' ' << toLight;
        }

        /// **The distribution is normalised**: `∫ D (n.h) dω` is one for every alpha, which in
        /// `s = cos²` is `∫ D(√s) π ds` over nought to one — worked out by hand, the antiderivative
        /// of `α² / (s (α² - 1) + 1)²` is `-α² / ((α² - 1) (s (α² - 1) + 1))`, which rises by
        /// exactly one across the interval. A million midpoint steps put two and a half thousand
        /// across the narrowest peak, `α² = 0.0025` wide, and the rule is then good to a part in
        /// ten million.
        TEST(RtxBrdfTest, theDistributionProjectsToOne)
        {
            constexpr std::uint32_t steps = 1000000;
            for (const float alpha : { 0.05f, 0.2f, 0.5f, 1.0f })
            {
                double sum = 0.0;
                for (std::uint32_t at = 0; at < steps; ++at)
                {
                    const float squared = (static_cast<float>(at) + 0.5f) / static_cast<float>(steps);
                    sum += static_cast<double>(Shaders::ggxDistribution(alpha, std::sqrt(squared)));
                }

                EXPECT_NEAR(sum * static_cast<double>(Shaders::PI) / steps, 1.0, 1e-5) << alpha;
            }
        }
    }
}
