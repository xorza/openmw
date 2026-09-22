#version 460

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_tracing : require

// `gl_HitTriangleVertexPositionsEXT`, which is the stage's own reading of what `traversal.glsl`
// reads off a query.
#extension GL_EXT_ray_tracing_position_fetch : require

// The one closest-hit shader the trace's hit table names, compiled three times.
//
// **Picked by traversal and not by a branch.** `SceneAcceleration::placeRow` writes each
// instance's shader-table offset from its material kind, so the hardware follows an index to the
// stage for that kind — and the three stages are this module under three settings of the two
// constants below: which albedo `resolve` is allowed to build, and whether the surface is shaded
// as water. One module is how the three cannot come to disagree about a hit the launch can no
// longer see for itself.
//
// **`LAYERED` folds the layer stack's loop and the four tables it walks out of the two stages no
// terrain can reach.** `WATER` picks the water's answer, and a frame with no sea still binds that
// record — an instance's offset is its material's kind, and a scene can hold water the build was
// told to ignore — so `HAS_SEA` is what says whether it shades as water or as the solid it then is.

#include "camera.h"
#include "scene.h"
#include "visibility.h"

#include "lib/bindings.glsl"
#include "lib/frame.glsl"
#include "lib/hitrecord.glsl"
#include "lib/payload.glsl"
#include "lib/random.glsl"
#include "lib/shading.glsl"
#include "lib/traversal.glsl"
#include "lib/variants.glsl"
#include "lib/water.glsl"

/// The two the hit module is specialized on, after the frame's tuple in `variants.glsl`: whether
/// ground that kept its layer stack can reach this stage, and whether a hit is shaded as water.
/// `VisibilityPass` hands each of the three stages its pair.
layout(constant_id = 5) const bool LAYERED = false;
layout(constant_id = 6) const bool WATER = false;

layout(location = RTX_PAYLOAD) rayPayloadInEXT VisibilityPayload packed;
hitAttributeEXT vec2 barycentrics;

/// What this stage's own builtins say the ray found, in the form `resolve` takes.
///
/// **The cone is the eye's the record names**, opened over the distance the stage was handed:
/// `stageCone` says which eye and why.
///
/// @param bary the stage's `hitAttributeEXT`, which cannot be read through a function boundary and
///        so is passed in.
Hit stageHit(vec2 bary)
{
    vec3 corners[3];
    corners[0] = gl_HitTriangleVertexPositionsEXT[0];
    corners[1] = gl_HitTriangleVertexPositionsEXT[1];
    corners[2] = gl_HitTriangleVertexPositionsEXT[2];

    const Cone cone = stageCone();

    return committedHit(uint(gl_InstanceCustomIndexEXT), uint(gl_PrimitiveID), bary, gl_HitTEXT,
        cone.mWidth + cone.mSpread * gl_HitTEXT, corners, gl_ObjectToWorldEXT);
}

/// The pixel this invocation was launched for.
uvec2 stagePixel()
{
    return gl_LaunchIDEXT.xy;
}

/// Fills the payload in for an ordinary lit surface, and for a pane the launch has still to peel.
///
/// **Which of the two it is is decided here, because this is where the number is.** A pane is a
/// surface whose resolved opacity is under one, and that opacity is a texture read `resolve` has
/// just made — the launch would have to be handed the material row and read it again. What the
/// launch is handed instead is `mOpacity`, which is the whole of what it needs to peel.
///
/// **A pane gets direct light, the path's end for everything else, and a lamp sequence of its own.**
/// `bounceLight` draws from the pixel and nothing else, so a pane and the wall behind it would
/// bounce off the same numbers — which is the correlation `SEED_LAMPS_PANE` exists to keep out of the
/// direct term, and there is no such seed to hand a bounce. What a pane gets instead is what a
/// surface a water ray finds gets: `pathEnd`, dimmed by one occlusion ray of its own. A faded actor
/// is a stack of these, and drawn with no indirect term at all they stood in a room where nothing
/// bounced near them. One sequence per layer, because a stack of them shades beside itself as well:
/// the record says which layer this is, and `paneSeed` and `paneAmbientSeed` are the sequences that
/// layer draws from.
///
/// **The launch peels `PEEL_LAYERS` of them and the one after that is a solid.** Without that a
/// shader would look at its own opacity, find one more pane, and shade it as a pane however deep the
/// stack went — so a launch that had run out of layers would draw a hole through the world rather
/// than the surface standing in it.
void answerSolid(inout Answer answer, Surface surface)
{
    const uvec2 pixel = stagePixel();

    answer.mOpacity = surface.mOpacity;

    if (isSeenThrough(surface.mOpacity) && record.mLayer < PEEL_LAYERS)
    {
        const uint key = pixelKey(pixel);
        answer.mRadiance
            = shadeAtPathEnd(surface, key + paneAmbientSeed(record.mLayer), key + paneSeed(record.mLayer), PATH_SEEN);
        return;
    }

    // **The colour is replaced and the surface is not.** What this view changes is what a pixel is
    // painted with; the thing under it is the same Lambert surface, and saying otherwise hands every
    // reader of the guide a frame with no normals in it — which the wavelet reads as "no surface
    // anywhere" and the upscaler reconstructs accordingly.
    if (frame.mShowAlbedo != 0u)
    {
        answer.mRadiance = surface.mAlbedo;
        answer.mResponse = lambertResponse(surface);
        return;
    }

    shadeSolid(surface, pixel, answer.mRadiance, answer.mBounced, answer.mResponse);
}

/// Fills the payload in for a water surface, and for the ground showing through its last half metre.
///
/// **A pixel of water with no water under it is the ground it stands on, shaded as the ground is.**
/// The surface fades out over the last half metre of depth, and fading toward the bed shaded at the
/// far end of a path — `pathEnd`, the flat ambient — where the dry pixel beside it gathers a real
/// bounce is a waterline drawn as a line between blue and brown however well the surface over it is
/// faded: in fog the ambient is the recorded colour and a bounce finds the sky's. So the bed the dry
/// pixel would have found is traced and shaded the way that pixel is, and the fade mixes the two as
/// one pixel.
///
/// **From a hair short of the surface and not a hair past it.** The waterline is where the ground
/// crosses the plane, so along the last pixel of water the bed lies within the bias of the surface —
/// and a trace that started past it found nothing, kept the whole water, and drew the line it was
/// there to remove as a bright hair. Solids only: nothing solid is nearer than a surface the eye's
/// own trace found first, so what this finds is the bed.
void answerWater(inout Answer answer, Surface surface)
{
    const uvec2 pixel = stagePixel();
    const vec3 origin = gl_WorldRayOriginEXT;
    const vec3 direction = gl_WorldRayDirectionEXT;
    const Cone cone = stageCone();

    answer.mWater = true;

    const WaterShading water = shadeWater(surface, direction, pixel, cone);
    answer.mRadiance = water.mRadiance;
    answer.mResponse = water.mResponse;
    answer.mMirror = water.mMirror;

    const float shore = water.mShore;
    if (shore >= 1.0)
        return;

    // Drawn, because this is what the eye sees through the water.
    const Surface bed = trace(origin, direction, max(surface.mDistance - SHADOW_BIAS, 0.0), cone.mWidth,
        cone.mSpread, solidMask(frame.mRayMask), true);
    if (!bed.mHit)
        return;

    vec3 bedLight;
    SurfaceResponse lambert;
    shadeSolid(bed, pixel, bedLight, answer.mBounced, lambert);

    // The direct light and the response as a blend, and the bounce whole, since the albedo it is put
    // back against carries the share.
    answer.mRadiance = mix(bedLight, answer.mRadiance, shore);
    answer.mResponse = SurfaceResponse(normalize(mix(lambert.mNormal, answer.mResponse.mNormal, shore)),
        lambert.mDiffuse * (1.0 - shore), answer.mResponse.mSpecular * shore,
        mix(lambert.mRoughness, answer.mResponse.mRoughness, shore));
}

void main()
{
    Answer answer = noAnswer();

    const Surface surface
        = resolveFor(stageHit(barycentrics), gl_WorldRayOriginEXT, gl_WorldRayDirectionEXT, LAYERED);

    if (WATER && HAS_SEA)
        answerWater(answer, surface);
    else
        answerSolid(answer, surface);

    packed = packAnswer(answer);
}
