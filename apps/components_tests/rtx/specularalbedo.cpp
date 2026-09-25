#include <algorithm>
#include <cmath>
#include <cstdint>

#include <gtest/gtest.h>

#include <osg/Vec2f>

#include <components/rtx/shaders/brdf.h>
#include <components/rtx/specularalbedo.hpp>

#include "lobeintegrals.hpp"

namespace Rtx
{
    namespace
    {
        /// The first column whose node stands at `cosine` or past it.
        std::uint32_t firstColumnFrom(float cosine)
        {
            return static_cast<std::uint32_t>(std::ceil(Shaders::specularTableColumn(cosine)));
        }

        /// The Ray Reconstruction guide's `EnvBRDFApprox2`, as its scale on the reflectance at normal
        /// incidence and its bias before the green's gate: the fit Ray Tracing Gems chapter 32 made
        /// to a split-sum table. Transcribed from the Streamline programming guide.
        osg::Vec2f envBrdfApprox2(float alpha, float cosine)
        {
            const float x[4] = { 1.0f, cosine, cosine * cosine, cosine * cosine * cosine };
            const float y[4] = { 1.0f, alpha, alpha * alpha, alpha * alpha * alpha };

            const float biasOver
                = (0.99044f * x[0] - 1.28514f * x[1]) * y[0] + (1.29678f * x[0] - 0.755907f * x[1]) * y[1];
            const float biasUnder = (1.0f * x[0] + 2.92338f * x[1] + 59.4188f * x[3]) * y[0]
                + (20.3225f * x[0] - 27.0302f * x[1] + 222.592f * x[3]) * y[1]
                + (121.563f * x[0] + 626.13f * x[1] + 316.627f * x[3]) * y[3];
            const float scaleOver
                = (0.0365463f * x[0] + 3.32707f * x[1]) * y[0] + (9.0632f * x[0] - 9.04756f * x[1]) * y[1];
            const float scaleUnder = (1.0f * x[0] + 3.59685f * x[2] - 1.36772f * x[3]) * y[0]
                + (9.04401f * x[0] - 16.3174f * x[2] + 9.22949f * x[3]) * y[1]
                + (5.56589f * x[0] + 19.7886f * x[2] - 20.2123f * x[3]) * y[3];

            return osg::Vec2f(std::max(0.0f, scaleOver / scaleUnder), std::max(0.0f, biasOver / biasUnder));
        }

        /// **The table is the integral of the lobe the shader draws**, against a quadrature that
        /// shares nothing with its draws: at nodes from a cosine of 0.004 to square on and from
        /// half rough to roughest, both edges included. The smoother rows are the rule's to miss
        /// and not the table's — its even steps in the light's cosine are too coarse near the pole
        /// for an alpha under about 0.07.
        ///
        /// **5e-4 is the two errors added, with room.** At the grazing node the table's 4096
        /// visible-normal draws a node are 2.3e-4 off the integral drawn with 2^20, and the rule is
        /// 1.3e-4 off it the other way; elsewhere the two are within 1.6e-4 and 2e-5. A table drawn
        /// from the distribution alone is 4.1e-3 off at the grazing node, and fails it.
        TEST(RtxSpecularAlbedoTest, theTableIsTheIntegralOfTheLobe)
        {
            const SpecularAlbedo& table = SpecularAlbedo::shared();
            for (const std::uint32_t row : { 32u, 48u, 63u })
                for (const std::uint32_t column : { 4u, 21u, 42u, 63u })
                {
                    const float roughness = Shaders::specularTableRoughness(row);
                    const float cosine = Shaders::specularTableCosine(column);
                    const osg::Vec2f read = table.at(cosine, roughness);
                    const osg::Vec2f expected = Testing::lobeIntegrals(cosine, roughness);

                    EXPECT_NEAR(read.x(), expected.x(), 5e-4) << "climbing at " << cosine << ' ' << roughness;
                    EXPECT_NEAR(read.y(), expected.y(), 5e-4) << "whole at " << cosine << ' ' << roughness;
                }
        }

        /// **The smoothest lobe is a mirror.** At the roughness floor, `alpha = 0.002`, a lobe
        /// reflects all of what reaches it and Schlick's weight is taken at the eye's own angle: the
        /// whole is one and the climbing share `(1 - cos)^5`. From a cosine of a fifth, where masking
        /// is nothing at that alpha, the table is 1e-4 from both. Nearer grazing it is not, and
        /// should not be: the whole falls to 0.89 at a cosine of 0.002, which the nodes there hold.
        TEST(RtxSpecularAlbedoTest, theSmoothestLobeIsAMirror)
        {
            const SpecularAlbedo& table = SpecularAlbedo::shared();
            for (std::uint32_t column = firstColumnFrom(0.2f); column < Shaders::SPECULAR_TABLE_SIZE; ++column)
            {
                const float cosine = Shaders::specularTableCosine(column);
                const osg::Vec2f read = table.at(cosine, Shaders::specularTableRoughness(0));

                EXPECT_NEAR(read.x(), std::pow(1.0f - cosine, 5.0f), 2.5e-4) << cosine;
                EXPECT_NEAR(read.y(), 1.0f, 2.5e-4) << cosine;
            }
        }

        /// **The Ray Reconstruction guide's fit lands near the table away from grazing**, as its
        /// scale and its bias against the table's two shares. The fit is to another lobe's table —
        /// another masking term — and over the cosines from a half up the two are at most 0.053
        /// apart, at a roughness of one. What this catches is a channel swapped or a factor lost,
        /// which moves them by far more; toward grazing the lobes part by up to 0.31, and the
        /// guide's own number would not demodulate the light this lobe reflects.
        TEST(RtxSpecularAlbedoTest, theRayReconstructionFitLandsNearTheTableAwayFromGrazing)
        {
            const SpecularAlbedo& table = SpecularAlbedo::shared();
            for (std::uint32_t row = 0; row < Shaders::SPECULAR_TABLE_SIZE; ++row)
                for (std::uint32_t column = firstColumnFrom(0.5f); column < Shaders::SPECULAR_TABLE_SIZE; ++column)
                {
                    const float roughness = Shaders::specularTableRoughness(row);
                    const float cosine = Shaders::specularTableCosine(column);
                    const osg::Vec2f read = table.at(cosine, roughness);
                    const osg::Vec2f fitted = envBrdfApprox2(roughness * roughness, cosine);

                    EXPECT_NEAR(fitted.x(), read.y() - read.x(), 0.06) << "scale at " << cosine << ' ' << roughness;
                    EXPECT_NEAR(fitted.y(), read.x(), 0.06) << "bias at " << cosine << ' ' << roughness;
                }
        }
    }
}
