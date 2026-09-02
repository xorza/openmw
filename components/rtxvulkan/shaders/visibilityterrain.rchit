#version 460

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_query : require
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_ray_tracing_position_fetch : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

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

layout(location = RTX_PAYLOAD) rayPayloadInEXT VisibilityPayload answer;
hitAttributeEXT vec2 barycentrics;

void main()
{
    clearAnswer(answer);
    answerSolid(
        answer, resolveFor(stageHit(barycentrics), gl_WorldRayOriginEXT, gl_WorldRayDirectionEXT, true));
}
