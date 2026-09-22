#version 460

#extension GL_GOOGLE_include_directive : require

// A debug fragment, kept where it stands nearer than what the eye's ray met at its pixel.
//
// **The trace's depth and not a depth attachment**, because the picture has none: what is drawn
// under these lines was traced, and its distances are in `Channel::Depth` at the traced extent.
// The offset from the eye is interpolated across the primitive and measured here, so a long line
// running into a wall stops where the wall is rather than where its ends' distances say. In the
// display's own values, as the interface is, because the drawers painted their colours against a
// monitor.

#include "gbuffer.h"
#include "line.h"
#include "sets.h"

layout(push_constant, scalar) uniform Push
{
    LineConstants frame;
};

layout(set = SET_PASS, binding = 0, GBUFFER_DEPTH) uniform readonly image2D depth;

layout(location = 0) in vec3 inOffset;
layout(location = 1) in vec4 inColour;

layout(location = 0) out vec4 outColour;

void main()
{
    // The traced texel under this fragment of the picture, nearest and not filtered: a distance
    // is not a quantity that averages.
    const vec2 across = vec2(frame.mTraced) / vec2(frame.mCamera.mWidth, frame.mCamera.mHeight);
    const uvec2 traced = min(uvec2(gl_FragCoord.xy * across), frame.mTraced - 1u);

    // Hidden by its alpha and not discarded: under the pass's `Over` blend an alpha of nought
    // leaves the pixel as it was, and a `discard` here is a demote the device is not asked for.
    const float shown = float(length(inOffset) <= imageLoad(depth, ivec2(traced)).y);

    outColour = vec4(inColour.rgb, inColour.a * shown);
}
