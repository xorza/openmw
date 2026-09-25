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
// takes it at every light a surface is lit by and draws its bounce from it; `Rtx::SpecularAlbedo`
// integrates it over the hemisphere on the host, once, by the same draw, into the table the shader
// reads the lobe's energy back out of. A table integrated from a second copy of these would
// compensate a lobe the shader does not draw.

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

    /// What share of the facets the eye sees also see the light: Smith's height-correlated `G2 / G1`,
    /// at most one. **All of the lobe that a draw from `visibleNormal` leaves in its weight, the
    /// Fresnel term apart**: the draw's density over the light direction is `G1 D / (4 (n.v))`, and
    /// against `F D V (n.l)` the distribution, the reflection's Jacobian and the eye's `G1` cancel.
    ///
    /// Divided out by hand rather than taken as that quotient: `smithVisibility` times
    /// `4 (n.v) (n.l)` is `G2`, Smith's `G1` is `2 (n.v) / ((n.v) + a(v))`, and the quotient is
    /// `(n.l) ((n.v) + a(v)) / ((n.l) a(v) + (n.v) a(l))` for `a(c) = sqrt(c² (1 - α²) + α²)`, which
    /// divides by nothing that vanishes while the light is above the surface.
    RTX_SHADER float smithShadowingGivenMasking(float alpha, float toEye, float toLight)
    {
        const float squared = alpha * alpha;
        const float eye = sqrt(toEye * toEye * (1.0f - squared) + squared);
        const float light = sqrt(toLight * toLight * (1.0f - squared) + squared);
        return toLight * (toEye + eye) / (toLight * eye + toEye * light);
    }

    /// A microfacet normal drawn in proportion to how much of the surface the eye sees facing it —
    /// GGX's visible normals, `G1 max(v.h, 0) D(h) / (n.v)` — in the lobe's own frame, with the
    /// macrosurface normal along z: Dupuy and Benyoub 2023's spherical caps.
    ///
    /// **The visible normals, and not the distribution**, because a draw from them leaves
    /// `smithShadowingGivenMasking` in its weight, which is at most one. A draw from `D` leaves
    /// `(v.h) / (n.v)` as well, which has no bound toward grazing.
    ///
    /// **The caps are exact for GGX and take no branch.** Stretched by the alpha across the surface,
    /// the lobe is the hemisphere of normals, and the normals an eye sees of a hemisphere are the
    /// eye plus a point drawn evenly on the unit sphere's cap above minus the eye's height,
    /// normalised. Stretched back, that sum is the facet. Heitz 2018 draws the same normals off a
    /// disc it has to warp; the caps were measured up to 39% faster.
    ///
    /// @param eye unit, with a positive z.
    /// @param raised in `[0, 1)`: how far down the cap the point stands, by height, from its top.
    /// @param turn the point's azimuth about the normal, as its cosine and its sine.
    RTX_SHADER vec3 visibleNormal(vec3 eye, float alpha, float raised, vec2 turn)
    {
        const vec3 stretched = normalize(vec3(eye[0] * alpha, eye[1] * alpha, eye[2]));
        const float height = (1.0f - raised) * (1.0f + stretched[2]) - stretched[2];
        const float across = sqrt(clamp(1.0f - height * height, 0.0f, 1.0f));
        const vec3 cap = vec3(across * turn[0], across * turn[1], height) + stretched;
        return normalize(vec3(cap[0] * alpha, cap[1] * alpha, cap[2]));
    }

    /// How wide, across, the cone is that the lobe's reflected rays fill down to half their peak
    /// density, at normal incidence — or `widest`, where that is narrower.
    ///
    /// **Twice the half vector's angle, on each side of the mirror direction.** Seen square on, the
    /// density of the reflected direction is `D` at the half vector, and `D` falls to half its peak
    /// where `cos²θ (α² - 1) + 1 = √2 α²`: at `tan²θ = α² (√2 - 1) / (1 - √2 α²)`, exactly. So the
    /// cone is four times that angle. Past `α² = 1 / √2` the density never falls to half above the
    /// surface, and the cone is the widest there is.
    RTX_SHADER float ggxConeWidth(float alpha, float widest)
    {
        const float rootTwo = 1.41421356f;
        const float squared = alpha * alpha;
        const float left = 1.0f - rootTwo * squared;
        if (!(left > 0.0f))
            return widest;

        return min(4.0f * atan(alpha * sqrt((rootTwo - 1.0f) / left)), widest);
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
