// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_HITSTAGE_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_HITSTAGE_GLSL

// What every shader the trace's hit table names has in common: the hit its own stage already
// answered, and the two ways of filling the payload in.
//
// **Written once and compiled three times.** The three closest-hit shaders differ in one literal
// apiece — which albedo `resolve` is allowed to build, and whether the surface is shaded as water —
// and everything else about them is here. Three copies of this is how they would come to disagree
// about a hit the launch can no longer see for itself.

#include "camera.h"
#include "scene.h"

#include "bindings.glsl"
#include "frame.glsl"
#include "payload.glsl"
#include "shading.glsl"
#include "traversal.glsl"
#include "water.glsl"

/// What this stage's own builtins say the ray found, in the form `resolve` takes.
///
/// **The cone is the camera's, because only the camera's rays reach this table.** The launch traces
/// the eye's ray and the one behind a pane it peeled, and both leave the same point through the same
/// lens — so the width at the hit is the lens's own, opened over the distance the stage was handed.
/// Every other ray in the frame is an inline query inside a shader and never comes through here.
///
/// @param bary the stage's `hitAttributeEXT`, which cannot be read through a function boundary and
///        so is passed in.
Hit stageHit(vec2 bary)
{
    vec3 corners[3];
    corners[0] = gl_HitTriangleVertexPositionsEXT[0];
    corners[1] = gl_HitTriangleVertexPositionsEXT[1];
    corners[2] = gl_HitTriangleVertexPositionsEXT[2];

    const Cone cone = coneAt(frame.mCamera);

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
/// **A pane gets direct light and no bounce, and its own lamp sequence.** `bounceLight` draws from
/// the pixel and nothing else, so a pane and the wall behind it would bounce off the same numbers —
/// which is the correlation `SEED_LAMPS_PANE` exists to keep out of the direct term, and there is no
/// such seed to hand a bounce.
void answerSolid(inout VisibilityPayload answer, Surface surface)
{
    const uvec2 pixel = stagePixel();

    answer.mOpacity = surface.mOpacity;

    // The second surface of a pair is shaded as the solid it stands in for, whatever its own
    // opacity says. `ASK_BEHIND` is where that is argued.
    if (isSeenThrough(surface.mOpacity) && (answer.mAsked & ASK_BEHIND) == 0u)
    {
        answer.mRadiance = shadeSurface(surface, vec3(0.0), pixelKey(pixel) + SEED_LAMPS_PANE, PATH_SEEN);
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
/// The surface fades out over the last half metre of depth, and what it used to fade toward was the
/// bed shaded at the far end of a path — `pathEnd`, the flat ambient — where the dry pixel beside it
/// gathers a real bounce. In fog the two are far apart: the ambient is the recorded colour and a
/// bounce finds the sky's, so the waterline came back as a line between blue and brown however well
/// the surface over it had been faded. So the bed the dry pixel would have found is traced and
/// shaded the way that pixel is, and the fade mixes the two as one pixel.
///
/// **From a hair short of the surface and not a hair past it.** The waterline is where the ground
/// crosses the plane, so along the last pixel of water the bed lies within the bias of the surface —
/// and a trace that started past it found nothing, kept the whole water, and drew the line it was
/// there to remove as a bright hair. Solids only: nothing solid is nearer than a surface the eye's
/// own trace found first, so what this finds is the bed.
void answerWater(inout VisibilityPayload answer, Surface surface)
{
    const uvec2 pixel = stagePixel();
    const vec3 origin = gl_WorldRayOriginEXT;
    const vec3 direction = gl_WorldRayDirectionEXT;
    const Cone cone = coneAt(frame.mCamera);

    answer.mWater = true;

    float shore;
    answer.mRadiance = shadeWater(surface, direction, answer.mResponse, answer.mMirror, pixel, shore);

    if (shore >= 1.0)
        return;

    const Surface bed = trace(
        origin, direction, max(surface.mDistance - SHADOW_BIAS, 0.0), cone.mWidth, cone.mSpread, MASK_SOLID);
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

#endif
