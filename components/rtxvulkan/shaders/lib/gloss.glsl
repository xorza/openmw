#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_GLOSS_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_GLOSS_GLSL

// The specular half of a surface: the lobe `brdf.h` states, taken at each light a surface is lit by,
// and what the upscaler is told it reflects.
//
// **A surface with no reflectance has no specular half, and is not asked for one.** Every vanilla
// surface is that, so `glossOf` answers it with one test and every step after reads a flag that is
// false; in a scene with no map at all the test is `HAS_MAPS`, a constant, and the half is compiled
// out.

#include "brdf.h"
#include "bindings.glsl"
#include "traversal.glsl"
#include "variants.glsl"

/// The lobe's two integrals at a cosine to the eye and a perceptual roughness: the table's four
/// nearest cell centres blended, the edge held past the outer ones — `Rtx::SpecularAlbedo::at`,
/// which the tests hold this to.
vec2 specularAlbedoAt(float cosine, float roughness)
{
    const float last = float(SPECULAR_TABLE_SIZE - 1u);
    const vec2 cell = clamp(vec2(cosine, roughness) * float(SPECULAR_TABLE_SIZE) - 0.5, vec2(0.0), vec2(last));
    const uvec2 low = uvec2(cell);
    const uvec2 high = min(low + 1u, uvec2(SPECULAR_TABLE_SIZE - 1u));
    const vec2 part = cell - vec2(low);

    return (specularAlbedoCell(low.x, low.y) * (1.0 - part.x) + specularAlbedoCell(high.x, low.y) * part.x)
        * (1.0 - part.y)
        + (specularAlbedoCell(low.x, high.y) * (1.0 - part.x) + specularAlbedoCell(high.x, high.y) * part.x) * part.y;
}

/// What a glossy surface is to every light it is lit by, worked out once per surface: the lobe
/// depends on the light only through its direction.
struct Gloss
{
    /// Whether there is a specular half at all: a reflectance, and a normal that faces the ray. A
    /// normal map tilts its normal to face it — `facingRay` — and an interpolated normal on this
    /// content can still lean away, where the lobe has nothing to give back along the ray.
    bool mGlossy;

    vec3 mNormal;
    vec3 mToEye;
    float mToEyeCosine;

    /// `F0` and `F90`, `specularEdge`'s.
    vec3 mReflectance;
    float mEdge;

    float mAlpha;

    /// `specularCompensation`: the energy one scattering event loses, put back.
    vec3 mCompensation;

    /// The directional albedo the lobe reflects toward the eye, compensated: what the upscaler
    /// demodulates the specular half by.
    vec3 mAlbedo;
};

/// The surface's specular half, or a record whose `mGlossy` is false.
Gloss glossOf(Surface surface)
{
    Gloss gloss;
    gloss.mGlossy = false;
    gloss.mNormal = surface.mNormal;
    gloss.mToEye = -surface.mIncident;
    gloss.mToEyeCosine = dot(surface.mNormal, gloss.mToEye);
    gloss.mReflectance = surface.mSpecular;
    gloss.mEdge = 0.0;
    gloss.mAlpha = 1.0;
    gloss.mCompensation = vec3(1.0);
    gloss.mAlbedo = vec3(0.0);

    if (!HAS_MAPS || !(max(max(surface.mSpecular.r, surface.mSpecular.g), surface.mSpecular.b) > 0.0)
        || !(gloss.mToEyeCosine > 0.0))
        return gloss;

    gloss.mGlossy = true;
    gloss.mEdge = specularEdge(surface.mSpecular.g);
    gloss.mAlpha = ggxAlpha(surface.mRoughness);

    const vec2 table = specularAlbedoAt(gloss.mToEyeCosine, surface.mRoughness);
    gloss.mCompensation = specularCompensation(surface.mSpecular, table.y);
    gloss.mAlbedo = specularAlbedoOf(surface.mSpecular, gloss.mEdge, table.x, table.y) * gloss.mCompensation;

    return gloss;
}

/// What the lobe makes of light arriving along one direction.
struct Reflection
{
    /// `F D V (n.l)`, compensated: what a unit of irradiance square to the light sends to the eye.
    vec3 mLobe;

    /// The Fresnel term at the half vector, which is the share of that light the diffuse half does
    /// not get: glTF's `(1 - F)` on the diffuse.
    vec3 mFresnel;
};

/// The lobe at `towards`, the direction to the light's centre.
///
/// @param side what decides which side of the surface a light has to stand on — `litCosine`'s
///        argument, and for the same reason: a normal map does not move a light through the surface,
///        and nothing on the far side of a sheet is reflected.
Reflection reflectionAt(Gloss gloss, vec3 side, vec3 towards)
{
    const float toLight = dot(gloss.mNormal, towards);
    if (!(toLight > 0.0) || !(dot(side, towards) > 0.0))
        return Reflection(vec3(0.0), vec3(0.0));

    const vec3 halfway = normalize(gloss.mToEye + towards);
    const vec3 fresnel = fresnelSchlick(gloss.mReflectance, gloss.mEdge, schlickWeight(dot(gloss.mToEye, halfway)));
    const float lobe = ggxDistribution(gloss.mAlpha, max(dot(gloss.mNormal, halfway), 0.0))
        * smithVisibility(gloss.mAlpha, gloss.mToEyeCosine, toLight) * toLight;

    return Reflection(fresnel * gloss.mCompensation * lobe, fresnel);
}

#endif
