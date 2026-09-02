#version 460

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_query : require
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_ray_tracing_position_fetch : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

// A plain textured surface, shaded where it was found.
//
// **One of the three the trace's hit table names, picked by traversal and not by a branch.**
// `SceneAcceleration::placeRow` writes each instance's shader-table offset from its material kind,
// so the hardware follows an index to get here — and the reorder that ran just before this put the
// lanes of the warp on the same one.
//
// **`resolve` is told no terrain can arrive**, which compiles the layer stack's loop and the four
// tables it walks out of this shader. That is the register relief Stage 2 is for.

#include "lib/hitstage.glsl"

layout(location = RTX_PAYLOAD) rayPayloadInEXT VisibilityPayload answer;
hitAttributeEXT vec2 barycentrics;

void main()
{
    clearAnswer(answer);
    answerSolid(answer,
        resolveFor(stageHit(barycentrics), gl_WorldRayOriginEXT, gl_WorldRayDirectionEXT, false));
}
