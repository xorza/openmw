#version 460

#extension GL_GOOGLE_include_directive : require

// A debug vertex in the world, projected through the frame's camera: `rayAt` run backwards. A
// pixel's ray is `forward + right * u - up * v` for `u` and `v` in [-1, 1] on the image plane,
// so a point `d` from the eye lands at `u = (d . right) / |right|^2 / ahead` and
// `v = -(d . up) / |up|^2 / ahead`, with `ahead = d . forward` — which is Vulkan's clip space
// once `ahead` is the divide, and `+Y` down as the image is indexed. The depth is `1 - near /
// ahead`, nought at the near plane and short of one ever after, which clips what stands behind
// the eye and nothing else; the traced depth is what decides the rest, in the fragment stage.

#include "line.h"

layout(push_constant, scalar) uniform Push
{
    LineConstants frame;
};

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColour;

layout(location = 0) out vec3 outOffset;
layout(location = 1) out vec4 outColour;

void main()
{
    const vec3 offset = inPosition - frame.mOrigin;
    const float ahead = dot(offset, frame.mCamera.mForward);
    const float across = dot(offset, frame.mCamera.mRight) / dot(frame.mCamera.mRight, frame.mCamera.mRight);
    const float down = -dot(offset, frame.mCamera.mUp) / dot(frame.mCamera.mUp, frame.mCamera.mUp);

    gl_Position = vec4(across, down, ahead - frame.mNear, ahead);

    outOffset = offset;
    outColour = inColour;
}
