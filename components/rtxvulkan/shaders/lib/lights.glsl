// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_LIGHTS_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_LIGHTS_GLSL

// Which lamps could reach a point, and what one delivers at a distance.
//
// **The two halves every consumer of a lamp must agree on.** Three places accumulate lamps —
// a surface, the air, and a puff of smoke — and they differ in the cosine, the shadow ray
// and the phase function. What they may not differ in is the reach and the falloff.

#include "colour.h"
#include "scene.h"
#include "bindings.glsl"
#include "variants.glsl"
#include "random.glsl"
#include "traversal.glsl"

/// Which lamps one cell of the grid holds, as a range into `lightIndices`.
///
/// **A shading point should not have to ask every lamp in the cell whether it is near.** Walking
/// them all costs the same whether one contributes or none do — and the fog made that unaffordable
/// rather than merely wasteful, since a march asks twenty-four times per pixel where a surface asks
/// once per hit.
///
/// A lamp is binned into every cell its reach touches, so this range is complete: the distance test
/// each caller still makes is a refinement of the answer and never a correction to it. A cell
/// outside the grid is one no lamp can reach, which is why falling off the edge returns nothing
/// rather than clamping to the nearest.
///
/// **By cell rather than by point, because a walk along a ray has the cell already.** The stretch a
/// ray spends inside one cell is one list asked once, which is what `weighLampsAlong` is built on.
uvec2 lampsInCell(vec3 cell)
{
    if (any(lessThan(cell, vec3(0.0))) || any(greaterThanEqual(cell, vec3(grid.mSize))))
        return uvec2(0u, 0u);

    const uvec3 at = uvec3(cell);
    // `flat` is what this wants to be called, and GLSL reserves it for interpolation.
    const uint index = (at.z * grid.mSize.y + at.y) * grid.mSize.x + at.x;

    return uvec2(lightOffsets[index], lightOffsets[index + 1u]);
}

/// The same, for a caller holding a place instead of a cell.
uvec2 lampsReaching(vec3 position)
{
    return lampsInCell(floor((position - grid.mOrigin) * grid.mInverseCell));
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

/// The integral of `falloff` along a ray, over the stretch of it between `from` and `to`.
///
/// **The same window and the same inverse square, integrated instead of sampled.** Where there is
/// nothing else along the ray to sample — an even haze with no sun in it — a lamp's whole share of
/// the air is this, and it is the sum a march of that ray converges to rather than an estimate of
/// one. `fogUniformAlong` is the caller.
///
/// Exact, and it is exact because the integrand is a rational function of one quantity. With `s`
/// measured from the ray's closest approach to the lamp and `r^2 = h^2 + s^2`, `falloff` is
/// `(1 - (r/R)^4)^2 / (r^2 + 1)`; in units of the reach that is `(1 - q^2)^2 / (q + e)` with
/// `q = r^2/R^2` and `e = 1/R^2`, which divides out to a cubic in `q` and a remainder over the
/// divisor. The cubic is an even polynomial in `s` and the remainder is the `atan`.
///
/// @param perpendicular how far the lamp stands off the ray's line.
/// @param from where the stretch starts, measured from the closest approach and signed.
/// @param to where it ends, likewise.
float falloffAlong(float perpendicular, float from, float to, float reach)
{
    // **Clipped to the chord and not merely evaluated over the stretch.** Past the reach the window
    // is exactly zero, and the polynomial that stands for it there is not.
    const float chord = reach * reach - perpendicular * perpendicular;
    if (!(chord > 0.0))
        return 0.0;

    const float halfChord = sqrt(chord);
    const float low = clamp(from, -halfChord, halfChord) / reach;
    const float high = clamp(to, -halfChord, halfChord) / reach;
    if (!(high > low))
        return 0.0;

    // In units of the reach, where the chord runs from `bump` to one and the guard `falloff` keeps
    // against the singularity is this much of it.
    const float bump = perpendicular * perpendicular / (reach * reach);
    const float guard = 1.0 / (reach * reach);

    const float c2 = -guard;
    const float c1 = guard * guard - 2.0;
    const float c0 = guard * (2.0 - guard * guard);
    const float rest = (1.0 - guard * guard) * (1.0 - guard * guard);

    // The cubic, with `q = bump + s^2` expanded into powers of `s`. Its leading coefficient is one,
    // which is why there is no `c3` above.
    const float k0 = ((bump + c2) * bump + c1) * bump + c0;
    const float k2 = (3.0 * bump + 2.0 * c2) * bump + c1;
    const float k4 = 3.0 * bump + c2;

    const float root = sqrt(bump + guard);

    const float lowSquared = low * low;
    const float highSquared = high * high;

    const float below = low * (k0 + lowSquared * (k2 / 3.0 + lowSquared * (k4 / 5.0 + lowSquared / 7.0)))
        + rest * atan(low / root) / root;
    const float above = high * (k0 + highSquared * (k2 / 3.0 + highSquared * (k4 / 5.0 + highSquared / 7.0)))
        + rest * atan(high / root) / root;

    // The substitution measured `s` in reaches, and `dt` carries the reach back out.
    return (above - below) / reach;
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
/// **What a puff of smoke wants for its own falloff**, which no estimator over a layer can answer:
/// a lamp's intensity runs as one over the square of a distance that changes from sprite to sprite,
/// where whether it is *seen* changes slowly. So the sum is taken here and the seeing is asked once
/// — `spritesAlong` says why.
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

/// One lamp held out of all the ones that could reach a point, and what it stands for.
///
/// **A reservoir is one candidate and the weight of everything it beat.** That second number is what
/// makes the estimator unbiased rather than merely cheap: the one held is divided by the chance it
/// was held, which is its own weight over the total, so a dim lamp that happens to win still speaks
/// for the whole cell.
///
/// A record rather than four locals because it is what would get carried, if carrying it were worth
/// anything: a reservoir from the previous frame or from a neighbour combines with this one by the
/// same rule that built it. **Measured before it was built and it is not worth building** — spending
/// a shadow ray on every lamp instead of choosing one is 0.03% better at Seyda Neen's customs office
/// and 0.32% at Wolverine Hall, and perfect selection cannot beat that.
struct Reservoir
{
    /// Where the ray this buys leaves from — a shading point, a step of a fog march, or a sprite.
    vec3 mFrom;

    /// What the lamp held would deliver there with nothing in the way.
    vec3 mRadiance;

    /// Where it stands, for the one shadow ray this buys, and how big it is — which is what that
    /// ray is aimed *somewhere on* rather than *at*.
    vec3 mTowards;
    float mDistance;
    float mRadius;

    /// The held lamp's own weight, and the weight of every candidate including it.
    float mWeight;
    float mTotal;
};

/// A reservoir that has weighed nothing, which buys no ray and delivers nothing.
Reservoir noLamps()
{
    return Reservoir(vec3(0.0), vec3(0.0), vec3(0.0), 0.0, 0.0, 0.0, 0.0);
}

/// Weighs every lamp reaching `from` into `kept`.
///
/// **One walk and three askers**, which is what stopped the fog and the sprites going unshadowed:
/// a surface, a step of a fog march and a layer of particles all want the same question — which of
/// these lamps is worth the one ray — and each used to answer it its own way or not at all.
///
/// **Called more than once builds one reservoir over all of it.** The fog weighs every step of a
/// march into the same one, so a single ray stands for the whole march rather than for one place in
/// it, and the estimator is unbiased over the sum it was accumulated from.
///
/// The cosine a diffuse surface takes a light at, with what a sheet takes from its far side.
///
/// **One statement of what "facing" means, used by the sun, the moons and every lamp.** A solid
/// takes the near side and nothing from behind; a sheet with a mask — a leaf — takes the far side
/// at `SHEET_TRANSMISSION` of the near, `GpuMesh::mSheet` having said so. Never both at once: a
/// direction is on one side of a plane or the other.
float litCosine(vec3 normal, vec3 towards, float transmission)
{
    const float cosine = dot(normal, towards);
    return max(cosine, 0.0) + transmission * max(-cosine, 0.0);
}

/// @param normal the surface's, or nothing at all for a point in a medium — the air and a puff have
///        no direction to face away from, so every lamp reaching them counts whole.
/// @param scale what this asker's own share of a lamp is worth: `INV_PI` for a Lambert surface,
///        `INV_FOUR_PI` times a step's weight for the air.
/// @param transmission what the far side of a sheet is worth, out of `Surface::mTransmission`.
///        Nought for a solid and for a point in a medium, which has no far side.
/// Offers one candidate to `kept`, already resolved to what it delivers at `from`.
///
/// **The reservoir's own rule, written once**, because two walks feed it: the point one below, and
/// the walk along a ray that `fogUniformAlong` takes. A second copy of this is a second chance for
/// the two to disagree about what unbiased means.
void considerLamp(inout Reservoir kept, inout uint state, vec3 from, vec3 unshadowed, Lamp lamp)
{
    // A scalar to weigh a colour by, which is what a target function has to be. The luminance,
    // because what it decides is which lamp this pixel would most notice the loss of.
    const float weight = dot(unshadowed, LUMINANCE_WEIGHTS);
    if (!(weight > 0.0))
        return;

    kept.mTotal += weight;

    // Hold the newcomer with probability `weight / total`, which leaves each candidate held in
    // proportion to its weight however many follow it — one-deep reservoir sampling.
    if (randomNext(state) * kept.mTotal <= weight)
    {
        kept.mFrom = from;
        kept.mRadiance = unshadowed;
        kept.mTowards = lamp.mTowards;
        kept.mDistance = lamp.mDistance;
        kept.mRadius = lamp.mRadius;
        kept.mWeight = weight;
    }
}

void weighLamps(inout Reservoir kept, inout uint state, vec3 from, vec3 normal, float scale, float transmission)
{
    const bool facing = dot(normal, normal) > 0.0;

    const uvec2 near = lampsReaching(from);
    for (uint i = near.x; i < near.y; ++i)
    {
        const Lamp lamp = lampAt(lights[lightIndices[i]], from);
        if (!(lamp.mReaching > 0.0))
            continue;

        const float cosine = facing ? litCosine(normal, lamp.mTowards, transmission) : 1.0;
        if (cosine <= 0.0)
            continue;

        considerLamp(kept, state, from, lamp.mIntensity * (cosine * lamp.mReaching * scale), lamp);
    }
}

/// What the world leaves of the lamp a reservoir held, from none of it to all.
///
/// **The one ray**, aimed somewhere on the lamp rather than at it. Nothing is traced where every
/// lamp was faced away from or out of reach, which is most of the frame.
///
/// It stops at whichever is further back from the centre — the lamp's own surface, or the unit of
/// clearance every shadow ray already keeps — so a source with a size never reaches inside itself
/// and one without behaves exactly as it did.
float lampVisible(Reservoir kept, vec2 draw)
{
    if (!(kept.mWeight > 0.0))
        return 1.0;

    const vec3 towards = coneDirection(kept.mTowards, min(kept.mRadius / kept.mDistance, 1.0), draw);

    return lightThrough(kept.mFrom, towards, kept.mDistance - max(kept.mRadius, SHADOW_BIAS));
}

/// What every lamp a reservoir stands for delivers, once the one it held has been traced to.
vec3 lampsThrough(Reservoir kept, vec2 draw)
{
    if (!(kept.mWeight > 0.0))
        return vec3(0.0);

    return kept.mRadiance * (kept.mTotal / kept.mWeight) * lampVisible(kept, draw);
}

#endif
