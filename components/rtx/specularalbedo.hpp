#pragma once

#include <span>
#include <vector>

#include <osg/Vec2f>

namespace Rtx
{
    /// The GGX lobe of `shaders/brdf.h` integrated over the hemisphere, once, into a
    /// `SPECULAR_TABLE_SIZE` square of nodes over the square root of the cosine to the eye and the
    /// perceptual roughness. Two integrals a node: `∫ w D V (n.l) dl` with `w` Schlick's weight, and
    /// `∫ D V (n.l) dl`.
    /// `specularAlbedoOf` makes a surface's directional albedo out of the pair for any reflectance,
    /// and `specularCompensation` its scale for the energy one scattering event loses.
    ///
    /// **Integrated rather than fitted.** The fits in use — Karis's, and `EnvBRDFApprox2` in the Ray
    /// Reconstruction guide — are fits to a lobe with another masking term, and a compensation off a
    /// lobe the shader does not draw puts back energy it did not lose. The table is the integral of
    /// the functions the shader calls, and a test holds it to a quadrature of its own.
    class SpecularAlbedo
    {
    public:
        /// The one table, built on first use a row a thread, in about 15 ms: paid once and not per
        /// pipeline.
        static const SpecularAlbedo& shared();

        SpecularAlbedo(const SpecularAlbedo&) = delete;
        SpecularAlbedo& operator=(const SpecularAlbedo&) = delete;

        /// The two integrals a node, node by node along the cosine and row by row along the
        /// roughness. Interleaved, so the pair one lookup reads sits together.
        std::span<const float> getValues() const { return mValues; }

        /// What the shader's lookup answers at `cosine` and `roughness`: the four nodes around the
        /// point blended, in the square root of the cosine. Read by the tests and by nothing else,
        /// which hold the device to it.
        osg::Vec2f at(float cosine, float roughness) const;

    private:
        SpecularAlbedo();

        std::vector<float> mValues;
    };
}
