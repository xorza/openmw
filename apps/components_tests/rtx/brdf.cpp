#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Vec2f>
#include <osg/Vec3f>

#include <components/rtx/radicalinverse.hpp>
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

        /// Smith's masking for GGX, `G1 = 1 / (1 + Λ)` with `Λ = (sqrt(1 + α² tan²θ) - 1) / 2`: the form
        /// `brdf.h` does not write, in double, for the tests to hold its forms to.
        double smithMasking(double alpha, double cosine)
        {
            const double tangent = (1.0 - cosine * cosine) / (cosine * cosine);
            return 2.0 / (1.0 + std::sqrt(1.0 + alpha * alpha * tangent));
        }

        /// **The lobe is reciprocal**: its visibility term is the same with the eye and the light
        /// swapped, to the rounding of a sum taken in the other order. **And what a visible-normal
        /// draw leaves of it is the quotient it stands for**: `G2 / G1` with `G2` the visibility
        /// times `4 (n.v) (n.l)` and `G1` through `Λ`, so nothing is shared with the closed form but
        /// the visibility.
        TEST(RtxBrdfTest, theSmithTermsAreReciprocalAndAgree)
        {
            for (const float alpha : { 0.002f, 0.1f, 0.5f, 1.0f })
                for (const float toEye : { 0.05f, 0.3f, 0.7f, 1.0f })
                    for (const float toLight : { 0.02f, 0.4f, 0.9f })
                    {
                        EXPECT_FLOAT_EQ(Shaders::smithVisibility(alpha, toEye, toLight),
                            Shaders::smithVisibility(alpha, toLight, toEye))
                            << alpha << ' ' << toEye << ' ' << toLight;

                        const double eye = toEye;
                        const double light = toLight;
                        const double shadowing
                            = static_cast<double>(Shaders::smithVisibility(alpha, toEye, toLight)) * 4.0 * eye * light;
                        EXPECT_NEAR(Shaders::smithShadowingGivenMasking(alpha, toEye, toLight),
                            shadowing / smithMasking(alpha, eye), 1e-6)
                            << alpha << ' ' << toEye << ' ' << toLight;
                    }
        }

        /// `∫ max(a + b cos φ, 0) dφ` over `[from, to]` inside `[0, π]`, for `b` of nought or more: the
        /// integrand is positive up to `acos(-a / b)` and nought past it, and its antiderivative is
        /// `a φ + b sin φ`.
        double positiveCosineIntegral(double a, double b, double from, double to)
        {
            double end = to;
            if (b > 0.0)
                end = std::min(to, std::acos(std::clamp(-a / b, -1.0, 1.0)));
            else if (!(a > 0.0))
                return 0.0;

            if (!(end > from))
                return 0.0;

            return a * (end - from) + b * (std::sin(end) - std::sin(from));
        }

        /// **The visible normals are drawn by their density**, `G1 max(v.h, 0) D(h) / (n.v)`: a
        /// histogram of `visibleNormal`'s draws, bin by bin, against that density integrated over
        /// the bin.
        ///
        /// The bins are even in `s = tan²θ / (α² + tan²θ)` — the projected distribution's own
        /// cumulative share, so each holds some of the lobe at every alpha — and in the facet's
        /// azimuth from the eye's, folded about the eye's plane, which the density is symmetric
        /// about. `D cosθ dω` is `ds dφ / 2π` in them, so the density folded is
        /// `G1 max(v_z + v_x α τ cos φ, 0) / (π v_z)` per `ds dφ` with `τ = sqrt(s / (1 - s))`.
        /// Taking `s = sin²β` makes `τ = tan β` and `ds = 2 sin β cos β dβ`, which leaves
        /// `v_z sin 2β + 2 v_x α sin²β cos φ` and no singularity at the lobe's edge; the azimuth is
        /// integrated exactly by `positiveCosineIntegral` and `β` by a midpoint rule. `G1` is
        /// `smithMasking`'s, so the expectation shares nothing with the draw — and it integrates to
        /// one within 6e-7 in every case, which this also holds it to.
        ///
        /// **2e-4 a bin is twice the worst the draw is off, and a twenty-fifth of a wrong draw.**
        /// 2^18 Hammersley points a case — the table's own set — come within 1e-4 of every bin, where
        /// independent draws would scatter a bin of the mean mass by 1.2e-4 a standard deviation. A
        /// draw that ignores the eye, the distribution projected onto the normal, is 5e-3 to 5e-2 off
        /// wherever the eye is not square on.
        TEST(RtxBrdfTest, theVisibleNormalsAreDrawnByTheirDensity)
        {
            constexpr std::uint32_t samples = 1u << 18;
            constexpr std::uint32_t bins = 16;
            constexpr std::uint32_t steps = 64;
            constexpr double pi = 3.14159265358979323846;

            std::vector<double> drawn(std::size_t{ bins } * bins);
            for (const float alpha : { 0.1f, 0.5f, 1.0f })
                for (const float cosine : { 1.0f, 0.5f, 0.1f })
                {
                    const osg::Vec3f eye(std::sqrt(1.0f - cosine * cosine), 0.0f, cosine);
                    const double stretch = alpha;

                    std::fill(drawn.begin(), drawn.end(), 0.0);
                    for (std::uint32_t at = 0; at < samples; ++at)
                    {
                        const float turn = Shaders::TAU * (static_cast<float>(at) + 0.5f) / static_cast<float>(samples);
                        const osg::Vec3f facet = Shaders::visibleNormal(
                            eye, alpha, radicalInverse(at, 2), osg::Vec2f(std::cos(turn), std::sin(turn)));

                        const double x = facet.x();
                        const double y = facet.y();
                        const double z = facet.z();
                        const double share = (x * x + y * y) / (stretch * stretch * z * z + x * x + y * y);
                        const double azimuth = std::abs(std::atan2(y, x));

                        const auto row = std::min(static_cast<std::uint32_t>(share * bins), bins - 1);
                        const auto column = std::min(static_cast<std::uint32_t>(azimuth / pi * bins), bins - 1);
                        drawn[std::size_t{ row } * bins + column] += 1.0 / samples;
                    }

                    const double eyeX = eye.x();
                    const double eyeZ = eye.z();
                    const double masking = smithMasking(alpha, eyeZ);

                    double total = 0.0;
                    for (std::uint32_t row = 0; row < bins; ++row)
                    {
                        const double low = std::asin(std::sqrt(static_cast<double>(row) / bins));
                        const double high = std::asin(std::sqrt(static_cast<double>(row + 1) / bins));
                        for (std::uint32_t column = 0; column < bins; ++column)
                        {
                            double mass = 0.0;
                            for (std::uint32_t step = 0; step < steps; ++step)
                            {
                                const double beta = low + (high - low) * (step + 0.5) / steps;
                                const double sine = std::sin(beta);
                                mass += positiveCosineIntegral(eyeZ * std::sin(2.0 * beta),
                                    2.0 * eyeX * stretch * sine * sine, pi * column / bins, pi * (column + 1) / bins);
                            }
                            mass *= (high - low) / steps * masking / (pi * eyeZ);
                            total += mass;

                            EXPECT_NEAR(drawn[std::size_t{ row } * bins + column], mass, 2e-4)
                                << "alpha " << alpha << ", eye cosine " << cosine << ", bin " << row << ' ' << column;
                        }
                    }

                    EXPECT_NEAR(total, 1.0, 1e-6) << "alpha " << alpha << ", eye cosine " << cosine;
                }
        }

        /// **The cone's edge is where the distribution falls to half**, at a quarter of the width:
        /// `D(θ) / D(0) = α⁴ / (cos²θ (α² - 1) + 1)²`, in double. By hand at `α = 0.3`,
        /// `tan²θ = 0.09 (√2 - 1) / (1 - 0.09 √2) = 0.0427161`, so `θ = 0.2038091` and the width is
        /// `0.8152363`. Past `α² = 1 / √2` — 0.85 and 1 — the lobe never falls to half above the
        /// surface and the answer is the widest, and it is never wider than that.
        TEST(RtxBrdfTest, theConeWidthIsWhereTheDistributionFallsToHalf)
        {
            for (const float alpha : { 0.002f, 0.05f, 0.3f, 0.8f })
            {
                const double edge = static_cast<double>(Shaders::ggxConeWidth(alpha, 10.0f)) / 4.0;
                const double squared = static_cast<double>(alpha) * static_cast<double>(alpha);
                const double cosine = std::cos(edge);
                const double falloff = cosine * cosine * (squared - 1.0) + 1.0;

                EXPECT_NEAR(squared * squared / (falloff * falloff), 0.5, 1e-5) << alpha;
            }

            EXPECT_NEAR(Shaders::ggxConeWidth(0.3f, 10.0f), 0.8152363f, 1e-6f);
            EXPECT_EQ(Shaders::ggxConeWidth(0.3f, 0.5f), 0.5f);
            EXPECT_EQ(Shaders::ggxConeWidth(0.85f, 10.0f), 10.0f);
            EXPECT_EQ(Shaders::ggxConeWidth(1.0f, 1.0f), 1.0f);
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
