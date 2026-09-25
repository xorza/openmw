#ifndef OPENMW_COMPONENTS_RTX_SHADERS_BRDF_H
#define OPENMW_COMPONENTS_RTX_SHADERS_BRDF_H

#include "hosttypes.h"
#include "look.h"
#include "portable.h"
#include "scene.h"

// The surface every lit solid is: glTF 2.0's metal and roughness, a GGX lobe with Smith's
// height-correlated masking and Schlick's Fresnel over a Lambert base. A vanilla surface is this
// surface with a reflectance of nought, which `SPECULAR_EDGE_SCALE` makes reflect exactly nothing.
//
// **Shared, because two sides evaluate the lobe and they have to evaluate one lobe.** The shader
// takes it at every light a surface is lit by; `Rtx::SpecularAlbedo` integrates it over the
// hemisphere on the host, once, into the table the shader reads the lobe's energy back out of. A
// table integrated from a second copy of these would compensate a lobe the shader does not draw.

#ifdef RTX_HOST
namespace Rtx::Shaders
{
#endif

    /// How many cells the specular albedo table has along each of its axes, the cosine to the eye and
    /// the perceptual roughness. Each cell is the integral at its centre, and a lookup blends the four
    /// nearest and holds the edge beyond the outer centres, as a texture's clamped bilinear read does.
    const uint SPECULAR_TABLE_SIZE = 32u;

    /// GGX's alpha for a perceptual roughness: its square, the roughness a map paints being
    /// perceptually linear — glTF 2.0 and Filament. Held at `ROUGHNESS_FLOOR`.
    RTX_SHADER float ggxAlpha(float roughness)
    {
        const float held = max(roughness, ROUGHNESS_FLOOR);
        return held * held;
    }

    /// The GGX (Trowbridge-Reitz) distribution of microfacet normals, per steradian: how much of the
    /// surface faces a half vector `cosine` off the normal. Normalised so that its projection onto
    /// the macrosurface, `D (n.h)`, integrates to one over the hemisphere.
    RTX_SHADER float ggxDistribution(float alpha, float cosine)
    {
        const float squared = alpha * alpha;
        const float denominator = cosine * cosine * (squared - 1.0f) + 1.0f;
        return squared * INV_PI / (denominator * denominator);
    }

    /// Smith's height-correlated masking and shadowing over the Cook-Torrance denominator,
    /// `G2 / (4 (n.v) (n.l))`, which is the form every lobe evaluation multiplies by: Heitz 2014, as
    /// Filament writes it. Symmetric in its two cosines, which is the lobe's reciprocity.
    RTX_SHADER float smithVisibility(float alpha, float toEye, float toLight)
    {
        const float squared = alpha * alpha;
        const float eye = toLight * sqrt(toEye * toEye * (1.0f - squared) + squared);
        const float light = toEye * sqrt(toLight * toLight * (1.0f - squared) + squared);
        return 0.5f / (eye + light);
    }

    /// Schlick's weight, `(1 - cosine)^5`: how far the reflectance at a half vector's angle has
    /// climbed from its value at normal incidence toward its value at grazing.
    RTX_SHADER float schlickWeight(float cosine)
    {
        const float away = clamp(1.0f - cosine, 0.0f, 1.0f);
        const float squared = away * away;
        return squared * squared * away;
    }

    /// The reflectance at grazing, from the reflectance at normal incidence's green —
    /// `SPECULAR_EDGE_SCALE` says why it is not one.
    RTX_SHADER float specularEdge(float green)
    {
        return clamp(SPECULAR_EDGE_SCALE * green, 0.0f, 1.0f);
    }

    /// Schlick's Fresnel, from the reflectance at normal incidence to the one at grazing.
    RTX_SHADER float fresnelSchlick(float normal, float edge, float weight)
    {
        return normal + (edge - normal) * weight;
    }

    /// The directional albedo of the lobe, from the table's two integrals at the eye's cosine and
    /// the roughness: `normal` times the share Schlick's weight leaves at normal incidence, and
    /// `edge` times the share it carries to grazing.
    ///
    /// @param climbing the table's first channel, `∫ w D V (n.l) dl` with `w` Schlick's weight.
    /// @param whole its second, `∫ D V (n.l) dl`: the albedo of a lobe that reflects all of what
    ///        reaches it.
    RTX_SHADER float specularAlbedoOf(float normal, float edge, float climbing, float whole)
    {
        return normal * (whole - climbing) + edge * climbing;
    }

    /// What the lobe is scaled by for the energy one scattering event loses: `1 + F0 (1 / E - 1)`,
    /// with `E` the table's `whole`. A single-scattering microfacet lobe drops the light that bounces
    /// between facets before it leaves, which grows with the roughness and is most of a metal's
    /// darkening at the rough end; Kulla and Conty 2017, as Turquin 2019 and Filament fold it into
    /// the one factor.
    RTX_SHADER float specularCompensation(float normal, float whole)
    {
        return 1.0f + normal * (1.0f / whole - 1.0f);
    }

#ifdef RTX_HOST
}
#endif

// What only the shading language reads: the same arithmetic over a colour's three reflectances.
#ifndef RTX_HOST

vec3 fresnelSchlick(vec3 normal, float edge, float weight)
{
    return normal + (vec3(edge) - normal) * weight;
}

vec3 specularAlbedoOf(vec3 normal, float edge, float climbing, float whole)
{
    return normal * (whole - climbing) + edge * climbing;
}

vec3 specularCompensation(vec3 normal, float whole)
{
    return 1.0 + normal * (1.0 / whole - 1.0);
}

#endif

#endif
