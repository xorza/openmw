#version 460

// The GUI's vertices arrive in clip space already: MyGUI multiplies widget pixels by the view size
// itself, so there is no matrix here and nothing for this shader to do but hand them on. The one
// difference between its clip space and Vulkan's — which way +Y points — is answered by a flipped
// viewport rather than a multiply here, so that this stays a pass-through.
//
// **Except the depth, which is dropped.** The pass has no depth attachment: widgets are drawn in
// the order MyGUI hands them over, and a vertex's z decides nothing here. It would decide whether
// the vertex exists at all, though — Vulkan clips z outside `[0, w]` — and what MyGUI's own
// sub-widgets write is a layer depth inside that range while OpenMW's book page writes minus one
// for every glyph (`mwgui/bookpage.cpp`, built against a GL projection that keeps it). That was
// every dialogue's text, every book and every scroll clipped away whole.


layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColour;
layout(location = 2) in vec2 inTexCoord;

layout(location = 0) out vec4 outColour;
layout(location = 1) out vec2 outTexCoord;

void main()
{
    gl_Position = vec4(inPosition.xy, 0.0, 1.0);

    outColour = inColour;
    outTexCoord = inTexCoord;
}
