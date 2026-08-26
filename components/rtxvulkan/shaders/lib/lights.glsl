// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_LIGHTS_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_LIGHTS_GLSL

// Which lamps could reach a point, and what one delivers at a distance.
//
// **The two halves every consumer of a lamp must agree on.** Three places accumulate lamps —
// a surface, the air, and a puff of smoke — and they differ in the cosine, the shadow ray
// and the phase function. What they may not differ in is the reach and the falloff.

#include "scene.h"
#include "bindings.glsl"

/// Which lamps could reach `position`, as a range into `lightIndices`.
///
/// **A shading point should not have to ask every lamp in the cell whether it is near.** Walking
/// them all costs the same whether one contributes or none do — and the fog made that unaffordable
/// rather than merely wasteful, since a march asks twenty-four times per pixel where a surface asks
/// once per hit.
///
/// A lamp is binned into every cell its reach touches, so this range is complete: the distance test
/// each caller still makes is a refinement of the answer and never a correction to it. A position
/// outside the grid is one no lamp can reach, which is why falling off the edge returns nothing
/// rather than clamping to the nearest cell's list.
uvec2 lampsReaching(vec3 position)
{
    const vec3 cell = floor((position - grid.mOrigin) * grid.mInverseCell);
    if (any(lessThan(cell, vec3(0.0))) || any(greaterThanEqual(cell, vec3(grid.mSize))))
        return uvec2(0u, 0u);

    const uvec3 at = uvec3(cell);
    // `flat` is what this wants to be called, and GLSL reserves it for interpolation.
    const uint index = (at.z * grid.mSize.y + at.y) * grid.mSize.x + at.x;

    return uvec2(lightOffsets[index], lightOffsets[index + 1u]);
}

/// How much of a light `distance` away arrives, per unit intensity.
///
/// An inverse square windowed to arrive at exactly zero where the light's reach ends. Morrowind's
/// reach is a hard cutoff, and merely clipping an inverse square leaves a visible ring on the floor
/// where it stops. The `+ 1` keeps the singularity at zero distance finite; a lamp is not a point.
float falloff(float distance, float reach)
{
    const float ratio = distance / reach;
    const float window = clamp(1.0 - ratio * ratio * ratio * ratio, 0.0, 1.0);
    return window * window / (distance * distance + 1.0);
}

/// One lamp as it arrives at a point.
///
/// **The reach test and the falloff, which is the whole of what a lamp is at a distance.** Three
/// places accumulate lamps — a surface, the air and a puff of smoke — and they differ in the cosine,
/// the shadow ray and the phase function. This is the part they may not differ in, so it is written
/// once and each of them weighs it its own way.
struct Lamp
{
    /// Unit, from the point toward the lamp. Zero where the lamp does not reach.
    vec3 mTowards;

    /// How far, in world units.
    float mDistance;

    /// The lamp's own intensity, carried so a caller needs nothing but this record.
    vec3 mIntensity;

    /// What share of that intensity arrives here, or nothing where the lamp does not reach.
    float mReaching;

    /// How big the glowing part is, in world units — carried for the one consumer that traces.
    ///
    /// **Visibility only, which is why it is beside the falloff rather than in it.** A surface
    /// draws its shadow ray from somewhere on a source this wide instead of from its centre; the
    /// air and a puff of smoke trace nothing and read past this, and all three still agree about
    /// the reach and the falloff, which is the whole point of the record.
    float mRadius;
};

Lamp lampAt(GpuLight lamp, vec3 position)
{
    const vec3 offset = lamp.mPosition - position;
    const float distance = length(offset);

    // **An early-out and not a rule**: the window in `falloff` is already exactly zero at and beyond
    // the reach, so this changes no pixel. What it saves is the shadow ray, which is the expensive
    // half of a light and the only reason the test is worth making at all. Zero distance is the
    // other half of it — a lamp standing exactly on the point has no direction to be lit from.
    if (distance >= lamp.mReach || distance <= 0.0)
        return Lamp(vec3(0.0), distance, lamp.mIntensity, 0.0, lamp.mRadius);

    return Lamp(offset / distance, distance, lamp.mIntensity, falloff(distance, lamp.mReach), lamp.mRadius);
}

/// What every lamp reaching a point delivers there, as irradiance and with nothing in the way.
///
/// **What the air and a puff of smoke both want**, which is the same question: neither has a normal
/// to face away from and neither is shadowed, so what is left is the sum. A surface asks a different
/// one and walks the lamps itself, because it spends a shadow ray on each and needs to stop early.
///
/// The isotropic factor is the caller's. It is one multiply on the sum rather than one per lamp,
/// which is one rounding rather than as many as the cell has lamps.
vec3 lampsAt(vec3 position)
{
    vec3 total = vec3(0.0);

    const uvec2 near = lampsReaching(position);
    for (uint i = near.x; i < near.y; ++i)
    {
        const Lamp lamp = lampAt(lights[lightIndices[i]], position);
        total += lamp.mIntensity * lamp.mReaching;
    }

    return total;
}

#endif
