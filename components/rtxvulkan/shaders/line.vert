#version 460

#extension GL_GOOGLE_include_directive : require

// A debug vertex in the world, projected through the frame's camera: `screenOf`, which is `rayAt`
// run backwards, handed to the rasterizer with `ahead` as the divide — which is Vulkan's clip
// space, and `+Y` down as the image is indexed. The depth is `1 - near / ahead`, nought at the
// near plane and short of one ever after, which clips what stands behind the eye and nothing
// else; the traced depth is what decides the rest, in the fragment stage.

#include "camera.h"
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
    const Screen screen
        = screenOf(frame.mCamera.mForward, frame.mCamera.mRight, frame.mCamera.mUp, offset, vec2(1.0));

    gl_Position = vec4(screen.mAt, screen.mAhead - frame.mNear, screen.mAhead);

    outOffset = offset;
    outColour = inColour;
}
