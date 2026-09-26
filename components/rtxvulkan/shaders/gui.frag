#version 460

#extension GL_GOOGLE_include_directive : require

// **Display-referred, and after the tone curve.** The GUI's colours and its atlases were authored
// against a monitor, so they are written out as they are, over a picture already in display values.

#include "sets.h"

layout(set = SET_PASS, binding = 0) uniform sampler2D uTexture;

layout(location = 0) in vec4 inColour;
layout(location = 1) in vec2 inTexCoord;

layout(location = 0) out vec4 outColour;

void main()
{
    outColour = texture(uTexture, inTexCoord) * inColour;
}
