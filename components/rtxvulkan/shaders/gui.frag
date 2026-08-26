#version 460

// **Display-referred, and the only shader here that is.** Everything before this pass works in
// scene radiance; the GUI's colours and its atlases were authored against a monitor, so they are
// written out as they are and the tone curve has already run.

layout(set = 0, binding = 0) uniform sampler2D uTexture;

layout(location = 0) in vec4 inColour;
layout(location = 1) in vec2 inTexCoord;

layout(location = 0) out vec4 outColour;

void main()
{
    outColour = texture(uTexture, inTexCoord) * inColour;
}
