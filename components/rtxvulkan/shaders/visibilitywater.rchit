#version 460

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_query : require
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_ray_tracing_position_fetch : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

// Water, shaded where it was found: a Fresnel mix of a reflection and a refraction, each traced from
// here as an inline query, and the ground showing through the last half metre at a shore.
//
// **The two extra traversals stay ray queries inside this shader.** Nothing here is traced by the
// pipeline and nothing recurses — what a reflection finds is resolved by the same `resolve` every
// other ray uses and shaded by `shadeSurface`, neither of which calls back into water.
//
// **A frame with no sea still binds this record**, because an instance's shader-table offset is its
// material's kind and a scene can hold water the build was told to ignore. `HAS_SEA` is what says
// so, and a build without it shades the surface as the solid it then is.
//
// `visibilitysurface.rchit` says why the three are three files.

#include "lib/hitstage.glsl"

layout(location = RTX_PAYLOAD) rayPayloadInEXT VisibilityPayload answer;
hitAttributeEXT vec2 barycentrics;

void main()
{
    clearAnswer(answer);

    const Surface surface
        = resolveFor(stageHit(barycentrics), gl_WorldRayOriginEXT, gl_WorldRayDirectionEXT, false);

    if (HAS_SEA)
        answerWater(answer, surface);
    else
        answerSolid(answer, surface);
}
