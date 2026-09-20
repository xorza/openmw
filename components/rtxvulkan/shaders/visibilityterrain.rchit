#version 460

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_tracing : require

// Ground, shaded where it was found.
//
// **The one of the three that keeps the layer stack.** A chunk near enough to be worth the sharpness
// carries four or five tiling textures, each masked by its own grid of weights, and `resolve` sums
// them at the hit. A chunk wide enough to be distant had the stack flattened into one texture and
// arrives here as a single fetch — both are terrain, so both come to this record and `resolve` tells
// them apart.
//
// `visibilitysurface.rchit` says why the three are three files.

#include "lib/hitstage.glsl"

layout(location = RTX_PAYLOAD) rayPayloadInEXT VisibilityPayload packed;
hitAttributeEXT vec2 barycentrics;

void main()
{
    Answer answer = noAnswer();
    answerSolid(
        answer, resolveFor(stageHit(barycentrics), gl_WorldRayOriginEXT, gl_WorldRayDirectionEXT, true));
    packed = packAnswer(answer);
}
