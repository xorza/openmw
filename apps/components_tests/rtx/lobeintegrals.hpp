#pragma once

#include <cmath>
#include <cstdint>

#include <osg/Vec2f>
#include <osg/Vec3f>

#include <components/rtx/shaders/brdf.h>

namespace Rtx::Testing
{
    /// The specular albedo table's two integrals at one eye cosine and roughness,
    /// `∫ w D V (n.l) dl` with `w` Schlick's weight and `∫ D V (n.l) dl`, by a midpoint rule over
    /// the light direction's cosine and azimuth: a second way to the same numbers, sharing nothing
    /// with the table's draws, and good to 1e-5 of its own convergence. Half the azimuths, doubled:
    /// the lobe is symmetric about the plane the eye stands in.
    ///
    /// **What a frame reflects at a cosine the table holds at its edge.** The table's cells are
    /// integrals at their centres and a lookup holds the outer ones, so a surface seen square on is
    /// compensated off a cosine of 0.984 while its bounce draws the lobe at one.
    inline osg::Vec2f lobeIntegrals(float cosine, float roughness)
    {
        constexpr std::uint32_t steps = 500;
        const float alpha = Shaders::ggxAlpha(roughness);
        const osg::Vec3f eye(std::sqrt(1.0f - cosine * cosine), 0.0f, cosine);

        double climbing = 0.0;
        double whole = 0.0;
        for (std::uint32_t up = 0; up < steps; ++up)
        {
            const float lightCosine = (static_cast<float>(up) + 0.5f) / static_cast<float>(steps);
            const float lightSine = std::sqrt(1.0f - lightCosine * lightCosine);
            for (std::uint32_t round = 0; round < steps; ++round)
            {
                const float azimuth = Shaders::PI * (static_cast<float>(round) + 0.5f) / static_cast<float>(steps);
                const osg::Vec3f light(lightSine * std::cos(azimuth), lightSine * std::sin(azimuth), lightCosine);
                osg::Vec3f half = eye + light;
                half.normalize();

                const float term = Shaders::ggxDistribution(alpha, half.z())
                    * Shaders::smithVisibility(alpha, cosine, lightCosine) * lightCosine;
                climbing += static_cast<double>(term * Shaders::schlickWeight(eye * half));
                whole += static_cast<double>(term);
            }
        }

        // Each cell of the rule is `d(cos) d(azimuth)` of solid angle, over half the circle twice.
        const double cell = 2.0 * static_cast<double>(Shaders::PI) / (static_cast<double>(steps) * steps);
        return osg::Vec2f(static_cast<float>(climbing * cell), static_cast<float>(whole * cell));
    }
}
