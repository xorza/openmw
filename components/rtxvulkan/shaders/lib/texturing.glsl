#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_TEXTURING_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_TEXTURING_GLSL

// Reading a texture at the level the ray's cone can resolve, and taking the painted-in
// lighting back out of it.
//
// Shared by everything that samples — a committed hit's colour, a candidate hit's cutout
// mask, and each layer of a piece of ground — which is what keeps them reading one level.

#include "scene.h"
#include "bindings.glsl"
#include "geometry.glsl"
#include "ground.glsl"

/// What the hit's own triangle contributes to a mip level, before any texture is named.
///
/// **One statement of the half of `coneBase` that every point on a hit's triangle shares.** A
/// terrain chunk builds four or five points there, one a layer; each needs the same square root,
/// the same divide by it and the same dot against the ray. The triangle and the ray are one pair
/// per hit, so this is worked out once and handed over.
///
/// **It measures at no time either way**, because `glslc` inlines these and removes some of the
/// repeat itself. A fact every point on the triangle needs is stated once regardless, and stating
/// it does not depend on the optimizer going on making that choice.
struct SurfaceCone
{
    /// Twice the triangle's area in the world. Nought for a degenerate one, which reads level zero.
    float mArea;

    /// A surface seen edge-on covers more of itself per pixel, and the cone's footprint on it grows
    /// by the same factor. Floored, because a grazing hit sends it to infinity.
    float mFacing;
};

/// @param crossed the triangle's edge cross product, whose length is twice its area. The texel area
///        in `coneLod` is doubled the same way, so the two cancel in the ratio.
SurfaceCone surfaceConeAt(vec3 crossed, vec3 direction)
{
    const float area = length(crossed);
    if (!(area > 0.0))
        return SurfaceCone(0.0, 1.0);

    return SurfaceCone(area, max(abs(dot(crossed / area, direction)), 1e-3));
}

/// A base at or below which every texture reads its finest level, whatever its own resolution adds.
///
/// **Both a sentinel and a threshold, and nothing has to know which a given point is.** A ray with
/// no cone is given this so that `coneLod` loads no texel count for it, and a base that reaches
/// it honestly would clamp to the finest level anyway — the largest `0.5 * log2(w * h)` a
/// `maxImageDimension2D` of 16384 allows is 14.
const float TEXTURE_FINEST_BASE = -64.0;

/// Every term of the level but the texture's own resolution: the texture-to-world area ratio of
/// the triangle, the cone's width where it landed, and the angle the surface presents.
///
/// **The ratio and not the coordinates it was measured from**, because a mesh's own texture
/// coordinates are not the only parameterisation a surface reads a sheet through: a sphere map is
/// indexed by where the eye is, and `spherePoint` measures the same ratio for it. One statement of
/// the level, two ways of reaching the area — written twice is how the two would come to disagree.
///
/// @param uvArea twice the triangle's area in the texture's own coordinates, against `cone.mArea`,
///        which is twice its area in the world. Doubled on both sides, so the two cancel.
/// @param coneWidth how wide the ray's cone is where it landed, or zero for a ray that carries no
///        cone at all — which is every shadow ray, and which reads the finest level.
float coneBaseOf(float uvArea, SurfaceCone cone, float coneWidth)
{
    if (!(coneWidth > 0.0) || !(cone.mArea > 0.0) || !(uvArea > 0.0))
        return TEXTURE_FINEST_BASE;

    // The frame's bias rides in the base, so every read through a `TexturePoint` — the albedo,
    // the mask, the emissive map, each ground layer — moves by the same levels. `mLevelBias`
    // says what it is for.
    return 0.5 * log2(uvArea / cone.mArea) + log2(coneWidth) - log2(cone.mFacing) + frame.mLevelBias;
}

/// The same for a triangle read through the mesh's own texture coordinates.
float coneBase(vec2 uv0, vec2 uv1, vec2 uv2, SurfaceCone cone, float coneWidth)
{
    const float uvArea = abs((uv1.x - uv0.x) * (uv2.y - uv0.y) - (uv2.x - uv0.x) * (uv1.y - uv0.y));

    return coneBaseOf(uvArea, cone, coneWidth);
}

/// Where on a texture a hit lands, and how coarse a level the ray's cone can still tell apart
/// there.
///
/// **One statement of a hit's place on a sheet, for every read made of that sheet.** A surface reads
/// its albedo, its opacity and its emissive map off one transform, so the three transformed corners
/// are worked out once per transform and handed to every read.
struct TexturePoint
{
    /// The hit, in the texture's own coordinates.
    vec2 mAt;

    /// The level any texture on this sheet is read at, before its own resolution is added.
    ///
    /// **The answer and not what it was made from.** The three transformed corners travelled to
    /// every sampler beside the surface's own cone so that each could take the same determinant
    /// again — eight floats through the call chain for one scalar that is the same for every map on
    /// the triangle, and a surface reads three of them.
    float mBase;
};

/// @param transform mesh texture coordinates to this texture's, as `uv * xy + zw`.
TexturePoint texturePoint(vec2 uv[3], vec2 bary, vec4 transform, SurfaceCone cone, float coneWidth)
{
    const vec2 corner0 = uv[0] * transform.xy + transform.zw;
    const vec2 corner1 = uv[1] * transform.xy + transform.zw;
    const vec2 corner2 = uv[2] * transform.xy + transform.zw;

    return TexturePoint(
        acrossTriangle(corner0, corner1, corner2, bary), coneBase(corner0, corner1, corner2, cone, coneWidth));
}

/// Where a sphere-mapped sheet is read, and how coarse a level the ray's cone can tell apart
/// there. `objects.vert`'s own coordinates: the eye-space view vector reflected about the eye-space
/// normal, folded onto the sheet, in whichever basis the caller shades in.
///
/// **The sheet is parameterised by the reflection, so its footprint is the normal's and not the
/// mesh's.** The two share nothing but the ray, so a `TexturePoint` built for the mesh's own
/// coordinates is wrong in either direction and by any amount: it blurs a flat blade, whose sheet
/// coordinate does not move at all, and it aliases the streaks on a curve that sweeps them.
///
/// **The ratio is closed form and not an estimate.** The sphere map `r.xy / (2 sqrt(2 + 2 r.z))` is
/// Lambert's azimuthal equal-area projection over four, so it carries solid angle to sheet area at
/// exactly one sixteenth, everywhere. Reflecting about a normal carries solid angle by `4 cos`, for
/// the angle between the ray and that normal — the shading normal the sheet is reflected about,
/// and not the plane's `SurfaceCone::mFacing`, which is how wide the ray's own footprint lies and
/// a different question. So the sheet area the triangle covers is `cos / 4` of the solid angle its
/// vertex normals span, and that span is the flat triangle their tips make.
///
/// **Solid angle is rotation invariant, so the normals never leave the mesh's own space.** Bringing
/// them across would want the object-to-world matrix, and `Hit` refuses to carry one: twelve floats
/// on every ray in the frame, for a sheet nearly no material has. A placement scaled unevenly would
/// move the span, which is the assumption the whole tree already makes of a normal — `Hit::mShading`
/// comes through the same matrix rather than its inverse transpose.
///
/// **What this leaves out is the ray's own turn across the triangle**, which moves the reflection
/// as much as the normal does. At 1080p and sixty degrees a pixel spans about a milliradian, so it
/// moves the sheet coordinate by 0.00025 — eight thousandths of a texel on a sheet 32 across, where
/// the normal's term is the whole of what a curve does.
///
/// @param normal the triangle's three vertex normals, as `triangleNormals` hands them: the mesh's
///        own space, and not yet unit.
/// @param shading where the hit's own normal points, in the caller's basis and unit.
/// @param direction the ray, in the caller's basis and unit.
TexturePoint spherePoint(vec3 normal[3], vec3 shading, vec3 direction, SurfaceCone cone, float coneWidth)
{
    const vec3 r = reflect(direction, shading);
    const float m = 2.0 * sqrt(r.x * r.x + r.y * r.y + (r.z + 1.0) * (r.z + 1.0));
    const vec2 at = r.xy / m + 0.5;

    // **Unit, because what the three tips span is a solid angle on the sphere** and a stored normal
    // is not one. A mesh that carries none holds zeros, which will not normalise: it shades from
    // its plane instead, whose normal stands still across the triangle, so the span is nought — and
    // the finest level is what a sheet coordinate that does not move across a triangle wants.
    const float shortest
        = min(min(dot(normal[0], normal[0]), dot(normal[1], normal[1])), dot(normal[2], normal[2]));
    if (!(shortest > 1e-8))
        return TexturePoint(at, TEXTURE_FINEST_BASE);

    const vec3 tip = normalize(normal[0]);
    const vec3 spread = cross(normalize(normal[1]) - tip, normalize(normal[2]) - tip);

    // Doubled, as `coneBaseOf` takes it: the tips' own triangle is half this cross, the solid angle
    // the reflections span is `4 cos` of that, and a sixteenth of it is sheet area.
    const float sheetArea = 0.25 * abs(dot(direction, shading)) * length(spread);

    return TexturePoint(at, coneBaseOf(sheetArea, cone, coneWidth));
}

/// Which mip one texture on that sheet should be read from.
///
/// Akenine-Moller's ray-cone formulation, split where JCGT 10(1) 2021 section 6 splits it: one term
/// in the texture's own resolution and one term in nothing else. A compute shader has no
/// derivatives, so this is the only thing standing between every fetch and level zero.
///
/// **The texture's own term is a load and not a header read.** `textureSize` asked the driver
/// for the slot's extent on every sample; the texel count is one word per slot for the life of the
/// slot, and the array's owner writes it beside the descriptor — `GpuTables::mTextureTexels`. The
/// logarithm stays here, over the same integer, so no level moves.
///
/// **The early answer is a read and not an arithmetic saving.** `lightThrough` says why a shadow
/// ray takes level zero, and what it saves is the load this makes; on a shadow ray the width is a
/// literal nought and the test folds.
float coneLod(uint slot, TexturePoint point)
{
    if (point.mBase <= TEXTURE_FINEST_BASE)
        return 0.0;

    return point.mBase + 0.5 * log2(float(textureTexelsAt(slot)));
}

/// The diffuse texel a hit landed on, read at the level its cone can resolve.
///
/// Shared by everything that asks: the colour of a committed hit, the mask of a candidate one, and
/// each layer of a piece of ground. Sharing it is what keeps them reading the same level — a cutout
/// resolved against a different mip than the surface it cuts would put the hole and the leaf in
/// different places.
vec4 sampleDiffuse(uint slot, TexturePoint point)
{
    return textureLod(textures[nonuniformEXT(slot)], point.mAt, coneLod(slot, point));
}

/// A tangent-space normal off a normal map, read at the level its cone can resolve: `2 rgb - 1`, as
/// `objects.frag` decodes it, and not unit — the frame it is carried through is normalised after.
///
/// **A map of two channels reads nought in blue**, and no map of three holds that: its blue is the
/// normal's height over the surface, from a half up. So a blue of nought is a BC5 or an RG map, and
/// the third is rebuilt from the two, as the rasterizer rebuilds it for exactly those formats.
vec3 sampleNormalMap(uint slot, TexturePoint point)
{
    const vec3 stored = sampleDiffuse(slot, point).rgb;
    const vec2 across = stored.xy * 2.0 - 1.0;
    const float up = stored.z > 0.0 ? stored.z * 2.0 - 1.0 : sqrt(max(1.0 - dot(across, across), 0.0));

    return vec3(across, up);
}

/// A `_spec` map's metalness and perceptual roughness — `GpuMaterial::mSpecular`. **The occlusion
/// in blue is not read**, because the traced bounce and `ambientReaching` already find what real
/// geometry occludes and the map would count it twice; nor the scattering in alpha, which the BC1
/// maps most of the content ships cannot carry.
vec2 sampleSpecularMap(uint slot, TexturePoint point)
{
    return sampleDiffuse(slot, point).rg;
}

/// The albedo a hit landed on, read at the level its cone can resolve, with the light painted
/// into the texture divided back out by the run's `mDelight` — `sampleAlbedoLod` says why.
vec3 sampleAlbedo(uint slot, TexturePoint point)
{
    return sampleAlbedoLod(slot, point.mAt, coneLod(slot, point), frame.mDelight);
}

/// How much of a terrain layer shows at `uv`, from the scene's grid of weights — `maskWeightIn`.
float maskWeight(GpuLayer layer, vec2 uv)
{
    return maskWeightIn(layer, uv, MaskTable(frame.mTables.mMasks));
}

#endif
