// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_PAYLOAD_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_PAYLOAD_GLSL

// What crosses between the launch and the shader an execute runs.
//
// **The whole of what the frame's tail needs, and nothing the hit object already answers.** A
// launch reads the hit itself, its distance and its instance row straight off the `hitObjectEXT`
// through `hitObjectIsHitEXT`, `hitObjectGetRayTMaxEXT` and `hitObjectGetInstanceCustomIndexEXT`,
// so not one word here is spent on them.
//
// **Live state is what a reorder costs**, and this is the live state: twenty-six words, a hundred
// and four bytes. The two fields that weigh most — the mirror a water pixel reflects and the
// response an upscaler demodulates by — are each read by the tail and neither can be worked out
// again from what is left.

#include "shading.glsl"
#include "traversal.glsl"
#include "water.glsl"

/// Where the shading payload below sits. A literal at every call, as the extension wants.
#define RTX_PAYLOAD 0

/// Where the payload traversal carries sits, which is a different and far smaller one.
///
/// **Traversal and shading invoke different shaders, so they are given different payloads.** The
/// any-hit shader a traversal reaches tests a cutout and reads nothing at all, where a closest-hit
/// shader fills in every field below — and the whitepaper's own example of when to split them is
/// this one. A traversal handed the shading payload pays register pressure for fields the shader it
/// runs never touches.
#define RTX_TRAVERSAL_PAYLOAD 1

/// The shader that ran was told it stands behind every layer the launch has left to peel.
///
/// **The launch peels `PEEL_LAYERS` of them and the one after that is a solid.** Without this a
/// chit would look at its own opacity, find one more pane, and shade it as a pane however deep the
/// stack went — so a launch that had run out of layers would draw a hole through the world rather
/// than the surface standing in it.
const uint ASK_BEHIND = 1u;

/// Where the layer of the peel this hit belongs to sits in `mAsked`, above the flags.
///
/// **Which layer and not only whether**, because the layers of a stack shade beside each other:
/// each opens a lamp reservoir, and a stack seeded alike would light every layer of a person from
/// the one lamp. `paneSeed` is where those sequences come from.
const uint ASK_LAYER_SHIFT = 1u;

/// What the launch tells a shader about the layer it is tracing for.
uint askForLayer(uint layer)
{
    return layer << ASK_LAYER_SHIFT;
}

/// Which layer of the peel the shader that reads this is standing at, counting the eye's own hit
/// as nought.
uint layerAsked(uint asked)
{
    return asked >> ASK_LAYER_SHIFT;
}

/// What the shader an execute ran hands back to the launch.
struct VisibilityPayload
{
    /// Handed in by the launch: `ASK_BEHIND`, and the layer of the peel above it.
    uint mAsked;

    /// What the surface sends back along the ray, before the pane, the water column, the air and
    /// the sprites the launch composites in front of it.
    vec3 mRadiance;

    /// The one bounce this hit gathered, kept apart because the upscaler demodulates it by the
    /// albedo in `mResponse` and multiplies the two back together afterwards.
    vec3 mBounced;

    /// What the shading model made of the surface, for the upscaler. `noResponse` where nothing was
    /// shaded — a pane, whose response is the surface behind it.
    SurfaceResponse mResponse;

    /// Water only, and `mFound` false everywhere else.
    WaterMirror mMirror;

    /// The sky only: how much of the star field the pixel shows through what the sky drew.
    float mSkyShown;

    /// How much of the surface the ray met is there, which is what the launch peels a pane on.
    float mOpacity;

    /// Whether what was shaded is water, which decides whether a reflection motion vector is
    /// written for the pixel.
    bool mWater;
};

/// Everything the launch reads, at what a shader that answered nothing would leave it.
///
/// **Called first by every shader the table names**, because a launch reads every field whatever
/// ran: a solid writes no mirror and the sky writes no response, and a field one shader skipped
/// would otherwise carry whatever the last pixel through that lane put there.
void clearAnswer(inout VisibilityPayload answer)
{
    answer.mRadiance = vec3(0.0);
    answer.mBounced = vec3(0.0);
    answer.mResponse = noResponse();
    answer.mMirror = WaterMirror(vec3(0.0), vec3(0.0), 0u, false);
    answer.mSkyShown = 0.0;
    answer.mOpacity = 1.0;
    answer.mWater = false;
}

#endif
