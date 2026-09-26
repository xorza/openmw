#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_LIGHTS_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_LIGHTS_GLSL

// Which lamps could reach a point, and what one delivers at a distance.
//
// **The two halves every consumer of a lamp must agree on.** Two places accumulate lamps — a
// surface and the air — and they differ in the cosine, the shadow ray and the phase function. What
// they may not differ in is the reach and the falloff. A puff of smoke reads the air's answer.

#include "colour.h"
#include "look.h"
#include "scene.h"
#include "sky.h"
#include "bindings.glsl"
#include "gloss.glsl"
#include "random.glsl"
#include "sky.glsl"
#include "traversal.glsl"
#include "variants.glsl"

/// The `source`th light in the sky, read off the frame: `SKY_SOURCE_SUN`, then the two moons.
///
/// **Read and not assembled.** The frame carries the sun as the record this returns, and a moon's
/// disc carries the same record as `MoonDisc::mSource` — so nothing here derives a fact the frame
/// already states. `SkySource` says what the three are one of.
SkySource skySourceAt(uint source)
{
    if (source == SKY_SOURCE_SUN)
        return frame.mSun;

    return frame.mMoons[source - SKY_SOURCE_MASSER].mSource;
}

/// What the world leaves of a light in the sky at a point, from none of it to all.
///
/// **One question, asked by every reader.** A surface, a froxel of the air and a step of a water
/// shaft each asked it their own way, and only the surface asked about the cloud deck — so a cloud
/// darkened the ground and not the fog over it or the beam under it. The deck is the one occluder
/// no ray finds, `cloudShadow` says why, and the ray is drawn across the disc's own penumbra: the
/// sun's for the reason `SUN_SHADOW_RADIUS` gives, a moon's at its own limb.
///
/// What the water over the point takes is not here: that is per channel and it is the caustic as
/// well as the absorption, and `lightThroughWater` is the one place it is answered.
///
/// @param draw one pair in `[0, 1)`, which aims the ray inside the disc's cone.
float skyVisible(SkySource sky, vec3 position, vec2 draw)
{
    return lightThrough(position, coneDirection(sky.mDirection, sky.mLimb, draw), frame.mReach)
        * cloudShadow(position, sky.mDirection);
}

/// The same for a caller that has an index and not a source.
float skyVisible(vec3 position, uint source, vec2 draw)
{
    return skyVisible(skySourceAt(source), position, draw);
}

/// Which lamps one cell of the grid holds, as a range into the light list.
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
/// ray spends inside one cell is one list asked once, which is what `weighLamps` is built on.
uvec2 lampsInCell(vec3 cell)
{
    if (any(lessThan(cell, vec3(0.0))) || any(greaterThanEqual(cell, vec3(frame.mLightGrid.mSize))))
        return uvec2(0u, 0u);

    const uvec3 at = uvec3(cell);
    // `flat` is what this wants to be called, and GLSL reserves it for interpolation.
    const uint index = (at.z * frame.mLightGrid.mSize.y + at.y) * frame.mLightGrid.mSize.x + at.x;

    return uvec2(lightListAt(index), lightListAt(index + 1u));
}

/// How many lamps one point may weigh before it stops.
///
/// **A budget, and not a limit a healthy grid reaches.** `Rtx::LightGrid` trades its cell size
/// against two budgets — the cells it may hold and the entries those cells may name — so a lamp
/// whose reach is unusually long makes the cell *coarser* rather than the table larger. Carried far
/// enough that is one cell holding every lamp in the scene, and every point inside it walking all of
/// them. At two million pixels, and sixty-four froxels for every column of the air, a walk that long
/// stops being a cost and becomes a kernel the driver cannot pre-empt: `Xid 109, CTX SWITCH
/// TIMEOUT`, and the device is reset under the frame.
///
/// **So the length of this walk is a property of the shader and never of the content.** Nothing the
/// world can hold makes a kernel here run for an unbounded time. What a scene past the budget loses
/// is the lamps a point weighs last, which are the ones its cell listed last — a bias, and one that
/// only appears where the alternative is no picture at all.
const uint LAMPS_AT_A_POINT = 256u;

/// The same range, cut to that budget.
uvec2 lampsWithin(uvec2 near)
{
    return uvec2(near.x, min(near.y, near.x + LAMPS_AT_A_POINT));
}

/// The same, for a caller holding a place instead of a cell.
uvec2 lampsReaching(vec3 position)
{
    return lampsInCell(floor((position - frame.mLightGrid.mOrigin) * frame.mLightGrid.mInverseCell));
}

/// How much of a light `distance` away arrives, per unit intensity.
///
/// An inverse square windowed to arrive at exactly zero where the light's reach ends. Morrowind's
/// reach is a hard cutoff, and merely clipping an inverse square leaves a visible ring on the floor
/// where it stops. What keeps the singularity at zero distance finite is `source`, below.
float falloff(float distance, float reach, float source)
{
    const float ratio = distance / reach;
    const float window = clamp(1.0 - ratio * ratio * ratio * ratio, 0.0, 1.0);

    // **The lamp's own extent is what the singularity is softened by, because that is what a lamp
    // is.** An inverse square is the field of a point, and a point has no field at itself: within a
    // flame's own radius the arithmetic runs away, and what it drew was a hard bright bead hanging
    // in the air wherever the fog sampled beside a lamp — a firefly, and not the glow of the thing
    // it belongs to. A sphere's irradiance flattens inside its own surface instead. One unit is the
    // floor, which is what a lamp carrying no size behaves as and what this read before.
    const float held = max(source, 1.0);

    return window * window / (distance * distance + held * held);
}

/// The integral of `falloff` along a ray, over the stretch of it between `from` and `to`.
///
/// **The same window and the same inverse square, integrated instead of sampled.** A froxel's whole
/// share of a lamp is this over its own stretch, and it is the sum a march of that stretch converges
/// to rather than an estimate of one. `lampsInAir` is the caller.
///
/// Exact, and it is exact because the integrand is a rational function of one quantity. With `s`
/// measured from the ray's closest approach to the lamp and `r^2 = h^2 + s^2`, `falloff` is
/// `(1 - (r/R)^4)^2 / (r^2 + source^2)`; in units of the reach that is `(1 - q^2)^2 / (q + e)` with
/// `q = r^2/R^2` and `e = source^2/R^2`, which divides out to a cubic in `q` and a remainder over the
/// divisor. The cubic is an even polynomial in `s` and the remainder is the `atan`.
///
/// @param perpendicular how far the lamp stands off the ray's line.
/// @param from where the stretch starts, measured from the closest approach and signed.
/// @param to where it ends, likewise.
float falloffAlong(float perpendicular, float from, float to, float reach, float source)
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
    const float held = max(source, 1.0);
    const float guard = held * held / (reach * reach);

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
/// **The reach test and the falloff, which is the whole of what a lamp is at a distance.** Two
/// places accumulate lamps — a surface and the air — and they differ in the cosine, the shadow ray
/// and the phase function. This is the part they may not differ in, so it is written once and each
/// of them weighs it its own way.
struct Lamp
{
    /// Unit, from the point toward the lamp. Zero where the lamp does not reach.
    vec3 mTowards;

    /// What share of that intensity arrives here, or nothing where the lamp does not reach.
    float mReaching;

    /// How far the lamp's centre is, which a fill's ball is measured against.
    float mDistance;
};

Lamp lampAt(GpuLight lamp, vec3 position)
{
    const vec3 offset = lamp.mPosition - position;
    const float squared = dot(offset, offset);

    // **An early-out and not a rule**: the window in `falloff` is already exactly zero at and beyond
    // the reach, so this changes no pixel. What it saves is the shadow ray, which is the expensive
    // half of a light and the only reason the test is worth making at all. Zero distance is the
    // other half of it — a lamp standing exactly on the point has no direction to be lit from.
    //
    // **On the square, before the root and the divide**, because most of a cell's list fails here:
    // a cell lists every lamp whose reach touches it, and at a point that is several lamps for each
    // one that reaches.
    if (squared >= lamp.mReach * lamp.mReach || squared <= 0.0)
        return Lamp(vec3(0.0), 0.0, 0.0);

    const float distance = sqrt(squared);

    return Lamp(offset / distance, falloff(distance, lamp.mReach, lamp.mSourceRadius), distance);
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
/// same rule that built it. **Not worth building**: spending a shadow ray on every lamp instead of
/// choosing one is a fraction of a per cent better, and perfect selection cannot beat that.
///
/// **The lamp is named and not copied.** What the one ray needs of it — where it stands, how wide
/// it is, how far short of it to stop — is read off its row when the ray is aimed, so nine words of
/// live state become one and a reservoir whose origin moved aims from where it now is for nothing.
struct Reservoir
{
    /// Where the ray this buys leaves from — a shading point, or a froxel of the air.
    vec3 mFrom;

    /// What the lamp held would deliver there with nothing in the way: `LightCandidate::mRadiance`.
    vec3 mRadiance;

    /// What the held lamp's light is to a surface's lobe, `LightCandidate::mSpecular` and
    /// `LightCandidate::mFresnel`: kept from the weighing, so the lobe is evaluated once per lamp.
    vec3 mSpecular;
    vec3 mFresnel;

    /// Which lamp, as a row of the light table.
    uint mLamp;

    /// The held lamp's own weight, and the weight of every candidate including it.
    float mWeight;
    float mTotal;
};

/// A reservoir that has weighed nothing, which buys no ray and delivers nothing.
Reservoir noLamps()
{
    return Reservoir(vec3(0.0), vec3(0.0), vec3(0.0), vec3(0.0), 0u, 0.0, 0.0);
}

/// The cosine a diffuse surface takes a light at, with what a sheet takes from its far side.
///
/// **One statement of what "facing" means, used by the sun, the moons and every lamp.** A solid
/// takes the near side and nothing from behind; a sheet with a mask — a leaf — takes the far side
/// at `SHEET_TRANSMISSION` of the near, `MESH_SHEET` having said so. Never both at once: a
/// direction is on one side of a surface or the other.
///
/// **Which side is one vector's answer and how much is another's, and the caller says which.** Four
/// hits in a hundred carry a normal more than sixty degrees off its own triangle, so the two
/// disagree often enough that picking wrongly is visible either way — and which of them is lying
/// depends on what the surface is. An open shape is a plane whose normals lean off it: read off the
/// normal, a wall panel takes a lamp standing behind it as a lamp in front, and having no far side
/// for the shadow ray to stop in it glows through itself. A closed shape is a solid the content
/// faceted, and its *facets* lean off the normals: read off the plane, whole triangles of a boulder
/// go black under a light its surface plainly faces. `Surface::mClosed` is what tells them apart.
///
/// @param side what decides which side of the surface a light has to stand on — `Surface::mSmooth`
///        for a closed shape and `Surface::mGeometric` for an open one. Zero has no meaning here:
///        an asker with no sides does not ask.
float litCosine(vec3 normal, vec3 side, vec3 towards, float transmission)
{
    const float cosine = dot(normal, towards);
    return dot(side, towards) > 0.0 ? max(cosine, 0.0) : transmission * max(-cosine, 0.0);
}

/// Which of three weights one draw picks, and what that pick is worth.
struct WeightedPick
{
    /// 0, 1 or 2, in the order the weights were given.
    uint mIndex;

    /// The picked weight over the total, or one where every weight is nought.
    float mChance;

    /// Whether the picked one holds all of the weight, and is taken whole rather than divided by a
    /// chance of one.
    bool mWhole;

    /// The weights' sum: nought where nothing is worth a ray.
    float mTotal;
};

/// Picks among three weights in proportion to them, with one draw in `[0, 1)`.
///
/// **Against the weights and never against a quotient.** `draw < first / total` says the same on
/// paper and not on the card: a divide is allowed 2.5 ULP here, `first / first` came back one ulp
/// under one, and the one draw in sixteen million that equals it picked a moon while she was down —
/// a chance of nought, a `0 / 0` in the froxel, and a NaN the fog's history then spread across the
/// frame in eight-pixel blocks. A product is correctly rounded and never exceeds `total`, and a
/// weight of nought is never the pick, so the fallback past every comparison is the last weight
/// that is anything. **And a pick holding all the weight is taken whole**, for the same reason: a
/// weight over itself need not be one either.
///
/// One statement for the surface's sky and the air's moons. A third weight of nought makes it a
/// pick between two.
WeightedPick pickByWeight(float first, float second, float third, float draw)
{
    const float total = first + second + third;
    const float scaled = draw * total;

    uint index = third > 0.0 ? 2u : (second > 0.0 ? 1u : 0u);
    if (first > 0.0 && scaled < first)
        index = 0u;
    else if (second > 0.0 && scaled < first + second)
        index = 1u;

    const float picked = index == 0u ? first : (index == 1u ? second : third);
    return WeightedPick(index, total > 0.0 ? picked / total : 1.0, !(picked < total), total);
}

/// What one light would deliver to a point with nothing in the way, and the weight it is drawn by.
///
/// **One record for a lamp and a sky source, for a surface and the air**, so what a light is worth
/// cannot be worked out two ways by two askers.
struct LightCandidate
{
    /// What the diffuse half receives, per unit albedo — or, in the air, the phase's share.
    vec3 mRadiance;

    /// What the lobe reflects toward the eye, whole. Nought on a surface with no lobe, and in the air.
    vec3 mSpecular;

    /// The lobe's Fresnel term at this light: the share of `mRadiance` the diffuse half does not get.
    vec3 mFresnel;

    float mWeight;
};

/// A light as a surface weighs it.
///
/// **One target for every light a surface draws between**: a glossy surface weighs a light by all it
/// sends back — the diffuse half net of what the lobe took, and the lobe — so a metal holds the
/// lights its highlights come from. Weighed by the cosine alone, it held them as often as the
/// lights behind its shoulder. A surface with no lobe keeps the cosine's weight per unit albedo,
/// which is the target every vanilla picture was drawn with: weighed by its albedo, a coloured light
/// would be held more or less often than before.
///
/// @param unshadowed what the diffuse half would receive per unit albedo.
/// @param plain the weight of a surface with no lobe, which is the luminance of `unshadowed` up to a
///        scale the whole draw shares. **Handed in, because each kind rounds it its own way and the
///        vanilla pictures were drawn with those roundings**: a lamp's is the luminance of the
///        product, and a sky source's the cosine times the luminance of the irradiance. Worked out
///        here the one way, the moons' pick flipped at a boundary and moved a night's pixels.
/// @param arriving the light's irradiance square to its direction, which the lobe reflects.
/// @param side what decides which side a light has to stand on, as `reflectionAt` takes it.
/// @param diffuse the surface's diffuse albedo.
LightCandidate surfaceCandidate(
    vec3 unshadowed, float plain, vec3 arriving, vec3 towards, Gloss gloss, vec3 side, vec3 diffuse)
{
    LightCandidate candidate = LightCandidate(unshadowed, vec3(0.0), vec3(0.0), plain);
    if (gloss.mGlossy)
    {
        const Reflection reflected = reflectionAt(gloss, side, towards);
        candidate.mSpecular = arriving * reflected.mLobe;
        candidate.mFresnel = reflected.mFresnel;
        candidate.mWeight = dot(unshadowed * diffuse * (1.0 - reflected.mFresnel) + candidate.mSpecular,
            LUMINANCE_WEIGHTS);
    }

    return candidate;
}

/// A light as a point of the air weighs it: its share, which has no lobe to add.
LightCandidate airCandidate(vec3 share)
{
    return LightCandidate(share, vec3(0.0), vec3(0.0), dot(share, LUMINANCE_WEIGHTS));
}

/// One sky source, weighed for a point that is about to draw between them.
///
/// **Named rather than kept in an array, because a computed index is a spill.** The three were held
/// in two `float[3]` locals and read back at the one the draw picked, and that index is
/// not one the compiler can fold: the surface stage of `visibilityhit.rchit.spv` carried six
/// `float[3]` variables in the function storage class, which is what a local array with a computed
/// index becomes on this hardware. Three named values cost registers instead.
///
/// **The spills are gone and the trace did not move.** That stage held six of those arrays and
/// holds none now, over three interleaved pairs that read the same to within the card's own drift.
/// Kept because a spill is what the compiler cannot undo for the next reader who adds a fourth
/// source.
struct SkyChoice
{
    SkySource mSky;

    /// What the surface makes of this source's direction, or nought where it is not asked.
    float mCosine;

    /// What it would deliver unshadowed, and the weight the draw is made on — `surfaceCandidate`'s,
    /// as a lamp's is.
    LightCandidate mLight;
};

/// Everything about one source that can be known before a ray is traced to it.
///
/// @param asked whether this source is one the caller wants at all — a sun that is down, or a moon
///        a bounce does not ask for.
/// @param diffuse the surface's diffuse albedo, which a glossy surface's weight reads.
SkyChoice skyChoiceAt(uint source, vec3 normal, vec3 side, float transmission, bool asked, Gloss gloss, vec3 diffuse)
{
    const SkySource sky = skySourceAt(source);
    const float cosine = asked ? litCosine(normal, side, sky.mDirection, transmission) : 0.0;

    // A source the surface does not face weighs nought and is never drawn, and its lobe is not worth
    // evaluating: in daylight that is both moons.
    LightCandidate light = LightCandidate(vec3(0.0), vec3(0.0), vec3(0.0), 0.0);
    if (cosine > 0.0)
        light = surfaceCandidate(sky.mIrradiance * (cosine * INV_PI), cosine * dot(sky.mIrradiance, LUMINANCE_WEIGHTS),
            sky.mIrradiance, sky.mDirection, gloss, side, diffuse);

    return SkyChoice(sky, cosine, light);
}

/// Offers one candidate to `kept`, already resolved to what it delivers at `from`.
///
/// **The reservoir's own rule, written once**, because two walks feed it: the point one below, and
/// the walk along a ray that `lampsInAir` takes. A second copy of this is a second chance for the
/// two to disagree about what unbiased means.
/// @param candidate what the lamp delivers and the weight it is drawn by — `surfaceCandidate` or
///        `airCandidate`. The weight is a scalar because a colour cannot be drawn in proportion to,
///        and positive wherever the candidate is anything the asker keeps.
/// @param lamp which row of the light table the candidate is.
void considerLamp(inout Reservoir kept, inout uint state, vec3 from, LightCandidate candidate, uint lamp)
{
    const float weight = candidate.mWeight;
    if (!(weight > 0.0))
        return;

    kept.mTotal += weight;

    // Hold the newcomer with probability `weight / total`, which leaves each candidate held in
    // proportion to its weight however many follow it — one-deep reservoir sampling.
    if (randomNext(state) * kept.mTotal <= weight)
    {
        kept.mFrom = from;
        kept.mRadiance = candidate.mRadiance;
        kept.mSpecular = candidate.mSpecular;
        kept.mFresnel = candidate.mFresnel;
        kept.mLamp = lamp;
        kept.mWeight = weight;
    }
}

/// Weighs every lamp reaching `from` into `kept`.
///
/// **The surface's walk of the grid, about a point.** The air's — `lampsInAir` — walks the same grid
/// along a ray, and the two feed one rule, `considerLamp`, so what unbiased means cannot come apart
/// between them.
///
/// @param normal the surface's, or nothing at all for a point in the air, which has no direction to
///        face away from, so every lamp reaching it counts whole.
/// @param side what decides which side a lamp has to stand on, beside the normal and going with it:
///        `litCosine` says why that is not always the same vector. Nothing at all where `normal` is.
/// @param scale what this asker's own share of a lamp is worth: `INV_PI` for a Lambert surface,
///        `INV_FOUR_PI` times a step's weight for the air.
/// @param transmission what the far side of a sheet is worth, out of `Surface::mTransmission`.
///        Nought for a solid and for a point in a medium, which has no far side.
/// @param gloss the surface's specular half, and `diffuse` its diffuse albedo, which weigh a lamp as
///        `surfaceCandidate` says.
void weighLamps(inout Reservoir kept, inout uint state, vec3 from, vec3 normal, vec3 side, float scale,
    float transmission, Gloss gloss, vec3 diffuse)
{
    const bool sided = dot(normal, normal) > 0.0;

    const uvec2 near = lampsWithin(lampsReaching(from));
    for (uint i = near.x; i < near.y; ++i)
    {
        const uint row = lightListAt(i);
        const GpuLight held = lightAt(row);
        const Lamp lamp = lampAt(held, from);

        // **Inside a fill's ball the cosine to the centre is blended out**, by how deep the point
        // stands, because the ball glows on every side of it there. `Rtx::Glow::makeLight` says what
        // a fill is: the rest of its weight is a lamp's, and so is its ray, which the ball's own
        // clearance keeps out of the ball. A factor of nought for a lamp, and not a branch, which
        // leaves a lamp's arithmetic the arithmetic it was.
        //
        // **And no test on the reach or the cosine before the offer.** A lamp out of reach carries
        // no direction and no share, so its weight below is nought, and `considerLamp` refuses a
        // weight of nought; two branches inside a loop of up to `LAMPS_AT_A_POINT` saved one dot
        // and one luminance apiece. The depth is selected and not divided for such a lamp, whose
        // distance is nought over a source that may be a point.
        const float faced = sided ? litCosine(normal, side, lamp.mTowards, transmission) : 1.0;
        const float depth = lamp.mReaching > 0.0
            ? float(held.mFill) * clamp(1.0 - lamp.mDistance / held.mSourceRadius, 0.0, 1.0)
            : 0.0;
        const float cosine = mix(faced, 1.0, depth);
        const vec3 unshadowed = held.mIntensity * (cosine * lamp.mReaching * scale);

        considerLamp(kept, state, from,
            surfaceCandidate(unshadowed, dot(unshadowed, LUMINANCE_WEIGHTS), held.mIntensity * lamp.mReaching,
                lamp.mTowards, gloss, side, diffuse),
            row);
    }
}

/// What the world leaves of the lamp a reservoir held, from none of it to all.
///
/// **The one ray**, aimed somewhere on the lamp. Asked only of a reservoir that holds one: both
/// callers refuse an empty one before this, and most of the frame is empty.
///
/// **It stops the clearance short of its own closest approach, and not of the centre.** Those are
/// the same length only for the ray down the middle. Take the clearance off the distance to the
/// centre and aim off-axis, and the ray runs *past* the source and into whatever fitting stands
/// around it — a lantern's frame, a sconce's bracket, a candle's holder — which is the densest
/// geometry anywhere near a lamp, and it comes back as fully shadowed. Taken against the closest
/// approach instead, every sampled direction ends at least the clearance away from the source
/// whatever angle it left at, and the grazing rays stop soonest of all.
///
/// **The clearance and the size are two numbers because they answer two questions**, and reading one
/// for both draws a black speckle over every lamp-lit wall in the game: aimed across the flame and
/// stopped at the flame, half the rays a wall sends end among the fitting and charge the whole
/// lamp to the pixel.
float lampVisible(Reservoir kept, vec2 draw)
{
    // Aimed from where the ray leaves and not from where the lamp was weighed, with no reach test:
    // a caller that moved its origin after weighing — the air does — still aims at the lamp it held.
    const GpuLight lamp = lightAt(kept.mLamp);
    const vec3 offset = lamp.mPosition - kept.mFrom;
    const float distance = length(offset);
    if (!(distance > 0.0))
        return 1.0;

    const vec3 axis = offset / distance;
    const vec3 towards = coneDirection(axis, min(lamp.mSourceRadius / distance, 1.0), draw);

    // How far along this direction the source stands beside it, which is where the ray is closest to
    // the lamp and so where the clearance has to be measured from.
    const float along = distance * dot(towards, axis);

    return lightThrough(kept.mFrom, towards, along - max(lamp.mClearance, SHADOW_BIAS));
}

/// Moves the ray a reservoir buys so that it leaves from `from` rather than from where the lamp it
/// holds was weighed.
///
/// **What a lamp is worth and where to ask whether it is seen are two questions.** A froxel weighs
/// its lamps over the whole of its own stretch, where a share is an integral and the point that
/// carries most of it is the closest approach — and then asks whether *the froxel* is shadowed,
/// which is a question about all of it and is best put from a point drawn anywhere inside. Over
/// frames that point walks the froxel, so a shadow's edge crossing one lands between two froxels as
/// something the filter averages rather than as a step eight pixels wide.
///
/// The selection is untouched, so the estimator stays what the walk that filled this says it is;
/// the lamp is named, so the ray aims itself from wherever it now leaves.
void aimLampFrom(inout Reservoir kept, vec3 from)
{
    kept.mFrom = from;
}

/// What every lamp a reservoir stands for delivers, once the one it held has been traced to.
///
/// @param share what the held lamp's own light is worth to the estimate: the reservoir's weight over
///        the held one's, times what the world left of it. Nought where nothing was held. What the
///        held lamp's lobe, `Reservoir::mSpecular`, is multiplied by, which keeps that estimate the
///        unbiased one this is.
vec3 lampsThrough(Reservoir kept, vec2 draw, out float share)
{
    share = 0.0;
    if (!(kept.mWeight > 0.0))
        return vec3(0.0);

    const float visible = lampVisible(kept, draw);
    share = (kept.mTotal / kept.mWeight) * visible;

    // Not `mRadiance * share`, which rounds differently. The share is dead code in a frame with no
    // specular half — every vanilla frame — and this is then the product vanilla pictures are held to.
    return kept.mRadiance * (kept.mTotal / kept.mWeight) * visible;
}

#endif
