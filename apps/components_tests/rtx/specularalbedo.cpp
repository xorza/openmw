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
        /// The centre of cell `at` along either axis of the table, where a lookup reads that cell
        /// alone: `(at + 0.5) / 32 * 32 - 0.5` is `at` exactly in float.
        float cellCentre(std::uint32_t at)
        {
            return (static_cast<float>(at) + 0.5f) / static_cast<float>(Shaders::SPECULAR_TABLE_SIZE);
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
        /// shares nothing with its draws. At cell centres, so a lookup reads the one cell. The
        /// grazing column is where drawing is hardest: the table's 4096 visible-normal draws a cell
        /// are 1.3e-4 off the converged quadrature there, and the 500-step rule is within 1e-5 of
        /// its own convergence — so 3e-4 is the table's error with room, and a table drawn from the
        /// distribution alone, 5e-3 off at this count, fails it.
        TEST(RtxSpecularAlbedoTest, theTableIsTheIntegralOfTheLobe)
        {
            const SpecularAlbedo& table = SpecularAlbedo::shared();
            for (const std::uint32_t row : { 8u, 16u, 31u })
                for (const std::uint32_t column : { 3u, 16u, 29u })
                {
                    const float roughness = cellCentre(row);
                    const float cosine = cellCentre(column);
                    const osg::Vec2f read = table.at(cosine, roughness);
                    const osg::Vec2f expected = Testing::lobeIntegrals(cosine, roughness);

                    EXPECT_NEAR(read.x(), expected.x(), 3e-4) << "climbing at " << cosine << ' ' << roughness;
                    EXPECT_NEAR(read.y(), expected.y(), 3e-4) << "whole at " << cosine << ' ' << roughness;
                }
        }

        /// **The smoothest lobe is a mirror.** At the roughness floor, `alpha = 0.002`, a lobe
        /// reflects all of what reaches it and Schlick's weight is taken at the eye's own angle: the
        /// whole is one and the climbing share `(1 - cos)^5`. Off grazing, where masking is nothing at
        /// that alpha, the table is 1e-4 from both.
        TEST(RtxSpecularAlbedoTest, theSmoothestLobeIsAMirror)
        {
            const SpecularAlbedo& table = SpecularAlbedo::shared();
            for (std::uint32_t column = 6; column < Shaders::SPECULAR_TABLE_SIZE; ++column)
            {
                const float cosine = cellCentre(column);
                const osg::Vec2f read = table.at(cosine, cellCentre(0));

                EXPECT_NEAR(read.x(), std::pow(1.0f - cosine, 5.0f), 2.5e-4) << cosine;
                EXPECT_NEAR(read.y(), 1.0f, 2.5e-4) << cosine;
            }
        }

        /// **The Ray Reconstruction guide's fit lands near the table away from grazing**, as its
        /// scale and its bias against the table's two shares. The fit is to another lobe's table —
        /// another masking term — and over the cosines from a half up the two are at most 0.050
        /// apart, at the roughest row. What this catches is a channel swapped or a factor lost,
        /// which moves them by far more; toward grazing the lobes part by up to 0.31, and the
        /// guide's own number would not demodulate the light this lobe reflects.
        TEST(RtxSpecularAlbedoTest, theRayReconstructionFitLandsNearTheTableAwayFromGrazing)
        {
            const SpecularAlbedo& table = SpecularAlbedo::shared();
            for (std::uint32_t row = 0; row < Shaders::SPECULAR_TABLE_SIZE; ++row)
                for (std::uint32_t column = Shaders::SPECULAR_TABLE_SIZE / 2; column < Shaders::SPECULAR_TABLE_SIZE;
                     ++column)
                {
                    const float roughness = cellCentre(row);
                    const float cosine = cellCentre(column);
                    const osg::Vec2f read = table.at(cosine, roughness);
                    const osg::Vec2f fitted = envBrdfApprox2(roughness * roughness, cosine);

                    EXPECT_NEAR(fitted.x(), read.y() - read.x(), 0.06) << "scale at " << cosine << ' ' << roughness;
                    EXPECT_NEAR(fitted.y(), read.x(), 0.06) << "bias at " << cosine << ' ' << roughness;
                }
        }
    }
}
