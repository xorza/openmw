#version 460

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_query : require
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_ray_tracing_position_fetch : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

// The sky, for a ray that reached nothing.
//
// **The whole of the sky's shading, out of the trace's own kernel.** It is fourteen percent of the
// pixels at the ship at Seyda Neen and it shares nothing with a surface: no instance row, no
// material, no lamp reservoir and no bounce. Here it is its own program with its own registers, and
// the launch that invoked it holds none of what it used.
//
// **What the star field shows through is this shader's to say.** `mSkyShown` is how much of the
// field the sky's own layers left, and the display pass cannot work it out for itself —
// `starsShown` in `bindings.glsl` says why.

#include "lib/bindings.glsl"
#include "lib/frame.glsl"
#include "lib/payload.glsl"
#include "lib/sky.glsl"
#include "lib/water.glsl"

layout(location = RTX_PAYLOAD) rayPayloadInEXT VisibilityPayload answer;

void main()
{
    clearAnswer(answer);

    const vec3 origin = gl_WorldRayOriginEXT;
    const vec3 direction = gl_WorldRayDirectionEXT;

    // **A ray that goes down from under the surface and finds nothing found water, and water is not
    // the sky.** `waterUnbounded` is the whole argument, and the launch asks it again for the column
    // the pixel is then seen through.
    if (waterUnbounded(false, origin, direction))
        return;

    if (frame.mTransparentBackground != 0u)
        return;

    answer.mRadiance = skyRadiance(origin, direction, pixelBlur(frame.mCamera), answer.mSkyShown);
}
