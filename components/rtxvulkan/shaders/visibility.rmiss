#version 460

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_tracing : require

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
#include "lib/variants.glsl"

layout(location = RTX_PAYLOAD) rayPayloadInEXT VisibilityPayload packed;

void main()
{
    // Before either early return: a ray that found water from under it, or a picture's background,
    // reached nothing all the same. `COUNT_HITS` in `variants.glsl` says why the count is taken here.
    if (COUNT_HITS)
        atomicAdd(counts.mMisses, 1u);

    Answer answer = noAnswer();

    const vec3 origin = gl_WorldRayOriginEXT;
    const vec3 direction = gl_WorldRayDirectionEXT;

    // **A ray that goes down from under the surface and finds nothing found water, and water is not
    // the sky.** `waterUnbounded` is the whole argument, and the launch asks it again for the column
    // the pixel is then seen through. A picture's background is nothing as well.
    if (!waterUnbounded(false, origin, direction) && frame.mTransparentBackground == 0u)
        answer.mRadiance = skyRadiance(origin, direction, pixelBlur(frame.mCamera), answer.mSkyShown);

    packed = packAnswer(answer);
}
