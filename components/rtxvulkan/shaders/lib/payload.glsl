#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_PAYLOAD_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_PAYLOAD_GLSL

// What crosses between the launch and the shader an execute runs.
//
// **The whole of what the frame's tail needs, and nothing the hit object already answers.** A
// launch reads the hit itself, its distance and its instance row straight off the `hitObjectEXT`
// through `hitObjectIsHitEXT`, `hitObjectGetRayTMaxEXT` and `hitObjectGetInstanceCustomIndexEXT`,
// so not one word here is spent on them.
//
// **What crosses the execute is what it costs**, and this is it: seventeen words. Every field the
// tail reads travels, and travels as small as the frame keeps it — the two albedos and the
// scalars as halves, which is the width of the channels they are stored in, and the normal and the
// mirror's direction as one word of octahedral halves apiece. What stays whole is the two
// radiances, because a reference is a sum of a thousand frames and a term rounded to a half
// before the sum does not average away, and the mirror's position, which is six figures of
// Morrowind's world that a half cannot hold. `Answer` is the same record unpacked, which is what
// the shaders write and the launch reads; `packAnswer` and `unpackAnswer` are the whole of the
// boundary.
//
// **Every word of it flows outwards.** The launch writes nothing here before an execute: what a
// closest-hit shader is told, it reads off its shader-table record, and `Shaders::HitRecord` says
// what measuring the other direction found.

#include "basis.glsl"
#include "records.glsl"

/// Where the shading payload below sits. A literal at every call, as the extension wants.
#define RTX_PAYLOAD 0

/// Where the payload traversal carries sits, which is a different and far smaller one.
///
/// **Traversal and shading invoke different shaders, so they are given different payloads.** The
/// any-hit shader a traversal reaches tests a cutout and reads nothing at all, where a closest-hit
/// shader fills in every field below — and the whitepaper's own example of when to split them is
/// this one. A traversal handed the shading payload pays register pressure for fields the shader it
/// runs never touches.
#define RTX_TRAVERSAL_PAYLOAD 1

/// What the shader an execute ran hands back to the launch, unpacked: the record a hit shader
/// fills in and the launch's tail reads, and never what crosses the execute itself.
struct Answer
{
    /// What the surface sends back along the ray, before the pane, the water column, the air and
    /// the sprites the launch composites in front of it.
    vec3 mRadiance;

    /// The one bounce this hit gathered, kept apart because the upscaler demodulates it by the
    /// albedo in `mResponse` and multiplies the two back together afterwards.
    vec3 mBounced;

    /// What the shading model made of the surface, for the upscaler. `noResponse` where nothing was
    /// shaded — a pane, whose response is the surface behind it.
    SurfaceResponse mResponse;

    /// Water only, and `mFound` false everywhere else.
    WaterMirror mMirror;

    /// The sky only: how much of the star field the pixel shows through what the sky drew.
    float mSkyShown;

    /// How much of the surface the ray met is there, which is what the launch peels a pane on.
    float mOpacity;

    /// Whether what was shaded is water, which decides whether a reflection motion vector is
    /// written for the pixel.
    bool mWater;
};

/// Everything the launch reads, at what a shader that answered nothing would leave it.
///
/// **What every shader the table names starts from**, because a launch reads every field
/// whatever ran: a solid writes no mirror and the sky writes no response, and a field one shader
/// skipped would otherwise carry whatever the last pixel through that lane put there.
Answer noAnswer()
{
    Answer answer;
    answer.mRadiance = vec3(0.0);
    answer.mBounced = vec3(0.0);
    answer.mResponse = noResponse();
    answer.mMirror = WaterMirror(vec3(0.0), vec3(0.0), 0u, false);
    answer.mSkyShown = 0.0;
    answer.mOpacity = 1.0;
    answer.mWater = false;

    return answer;
}

/// The record as it crosses the execute: seventeen words, laid out once here.
///
/// The flags word carries the mirror's instance in its low twenty-four bits — a custom index is
/// twenty-four bits wide — and three facts above them: whether the surface is water, whether the
/// mirror found anything, and whether the response carries a normal at all. The last is what a
/// pane and the sky leave nought, and nought has no direction to pack.
struct VisibilityPayload
{
    vec3 mRadiance;
    vec3 mBounced;
    vec3 mMirrorAt;

    /// The response's diffuse and specular, then its roughness beside the opacity: six halves and
    /// two more in four words.
    uvec4 mHalves;

    /// The stars' share, a half in the low bits; the high half is spare.
    uint mSkyShown;

    /// The response's normal and the mirror's direction, `packDirection` apiece.
    uint mNormal;
    uint mMirrorAlong;

    uint mFlags;
};

const uint ANSWER_INSTANCE_BITS = 0x00FFFFFFu;
const uint ANSWER_WATER = 1u << 31u;
const uint ANSWER_MIRROR_FOUND = 1u << 30u;
const uint ANSWER_HAS_NORMAL = 1u << 29u;

VisibilityPayload packAnswer(Answer answer)
{
    const bool hasNormal = dot(answer.mResponse.mNormal, answer.mResponse.mNormal) > 0.0;

    VisibilityPayload packed;
    packed.mRadiance = answer.mRadiance;
    packed.mBounced = answer.mBounced;
    packed.mMirrorAt = answer.mMirror.mAt;
    packed.mHalves = uvec4(packHalf2x16(answer.mResponse.mDiffuse.rg),
        packHalf2x16(vec2(answer.mResponse.mDiffuse.b, answer.mResponse.mSpecular.r)),
        packHalf2x16(answer.mResponse.mSpecular.gb),
        packHalf2x16(vec2(answer.mResponse.mRoughness, answer.mOpacity)));
    packed.mSkyShown = packHalf2x16(vec2(answer.mSkyShown, 0.0));
    packed.mNormal = hasNormal ? packDirection(answer.mResponse.mNormal) : 0u;
    packed.mMirrorAlong = answer.mMirror.mFound || answer.mWater ? packDirection(answer.mMirror.mAlong) : 0u;
    packed.mFlags = (answer.mMirror.mInstance & ANSWER_INSTANCE_BITS) | (answer.mWater ? ANSWER_WATER : 0u)
        | (answer.mMirror.mFound ? ANSWER_MIRROR_FOUND : 0u) | (hasNormal ? ANSWER_HAS_NORMAL : 0u);

    return packed;
}

Answer unpackAnswer(VisibilityPayload packed)
{
    const bool hasNormal = (packed.mFlags & ANSWER_HAS_NORMAL) != 0u;
    const vec2 diffuseRg = unpackHalf2x16(packed.mHalves.x);
    const vec2 diffuseBSpecularR = unpackHalf2x16(packed.mHalves.y);
    const vec2 specularGb = unpackHalf2x16(packed.mHalves.z);
    const vec2 roughnessOpacity = unpackHalf2x16(packed.mHalves.w);

    Answer answer;
    answer.mRadiance = packed.mRadiance;
    answer.mBounced = packed.mBounced;
    answer.mResponse = SurfaceResponse(hasNormal ? unpackDirection(packed.mNormal) : vec3(0.0),
        vec3(diffuseRg, diffuseBSpecularR.x), vec3(diffuseBSpecularR.y, specularGb), roughnessOpacity.x);
    answer.mMirror = WaterMirror(packed.mMirrorAt, unpackDirection(packed.mMirrorAlong),
        packed.mFlags & ANSWER_INSTANCE_BITS, (packed.mFlags & ANSWER_MIRROR_FOUND) != 0u);
    answer.mSkyShown = unpackHalf2x16(packed.mSkyShown).x;
    answer.mOpacity = roughnessOpacity.y;
    answer.mWater = (packed.mFlags & ANSWER_WATER) != 0u;

    return answer;
}

#endif
