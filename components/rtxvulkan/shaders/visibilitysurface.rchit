#version 460

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_tracing : require

// A plain textured surface, shaded where it was found.
//
// **One of the three the trace's hit table names, picked by traversal and not by a branch.**
// `SceneAcceleration::placeRow` writes each instance's shader-table offset from its material kind,
// so the hardware follows an index to get here.
//
// **`resolve` is told no terrain can arrive**, which compiles the layer stack's loop and the four
// tables it walks out of this shader.

#include "lib/hitstage.glsl"

layout(location = RTX_PAYLOAD) rayPayloadInEXT VisibilityPayload packed;
hitAttributeEXT vec2 barycentrics;

void main()
{
    Answer answer = noAnswer();
    answerSolid(answer,
        resolveFor(stageHit(barycentrics), gl_WorldRayOriginEXT, gl_WorldRayDirectionEXT, false));
    packed = packAnswer(answer);
}
