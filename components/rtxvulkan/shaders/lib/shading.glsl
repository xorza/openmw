// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_SHADING_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_SHADING_GLSL

// What an ordinary lit surface does with light: the direct sources it can ask about, the
// one bounce it traces for everything else, and the terms an upscaler demodulates by.

#include "colour.h"
#include "look.h"
#include "scene.h"
#include "bindings.glsl"
#include "frame.glsl"
#include "lights.glsl"
#include "random.glsl"
#include "sky.glsl"
#include "traversal.glsl"
#include "underwater.glsl"

/// Which end of a path a surface is being shaded at.
///
/// **Not a count of bounces.** What separates them is whether the result is looked at: a seabed
/// through water and a face in a mirror are each a bounce out and each is what the pixel shows, so
/// they are lit like anything else the eye can see. Only the hemisphere sample's far hit is a term
/// that nothing resolves on its own, and only there is a light worth dropping.
const uint PATH_SEEN = 0u;
const uint PATH_INDIRECT = 1u;

/// The *direct* light arriving at a point and turning back out of it, per unit albedo.
///
/// **Sources that can be asked where they are, and nothing else.** The sun and the lamps are each a
/// known direction and a shadow ray; everything that arrives by having bounced off something is
/// `bounceLight`'s, and adding a fill here as well would count that half twice.
///
/// One shadow ray per light that could reach at all, and none for a light the surface faces away
/// from — the two tests before it are what keep a cell's worth of lamps affordable.
///
/// @param side what decides which side of the surface a light has to stand on, which
///        `Surface::mClosed` picks between the plane and the shading normal. `litCosine` says why
///        neither answers for both.
/// @param footprint how wide the cone that found this point had grown, which is the scale the
///        caustics are allowed to resolve waves at.
/// @param seed which draw sequence the lamp reservoir steps. **One per depth of the path**, because
///        a bounce shades a second surface and two reservoirs stepping one sequence would keep
///        correlated lamps at both ends of it.
/// @param transmission what a light on the far side of the surface is worth to this side, which
///        is `Surface::mTransmission`: nought for a solid, `SHEET_TRANSMISSION` for a leaf.
/// @param path `PATH_SEEN` or `PATH_INDIRECT`. It decides whether the moons are asked at all, and
///        whether the rest of this is drawn at `INDIRECT_LIGHT_RATE` or spent on every hit.
vec3 gather(vec3 position, vec3 normal, vec3 side, float footprint, float transmission, uint seed, uint path)
{
    vec3 radiance = vec3(0.0);

    // **Drawn before anything else and out of a sequence of its own**: the ordering below is what
    // keeps a lamp arriving in the next cell from moving the penumbra of the one already there, and
    // a draw taken from that sequence would move every one of them.
    float rated = 1.0;
    if (path == PATH_INDIRECT && skyLights())
    {
        uint lit = randomSeed(seed + SEED_INDIRECT_LIGHT);
        if (randomNext(lit) >= INDIRECT_LIGHT_RATE)
            return radiance;

        rated = 1.0 / INDIRECT_LIGHT_RATE;
    }

    uint state = randomSeed(seed);

    // **Where on a source a shadow ray leaves from is drawn before anything is weighed**, so that it
    // does not depend on how many lamps the cell happened to hold: the two pairs sit at a fixed
    // place in the sequence and the reservoir's own draws follow them. Otherwise a lamp arriving in
    // the next cell along would move the penumbra of the one already there.
    const vec2 sunDraw = vec2(randomNext(state), randomNext(state));
    // **Three pairs are drawn and one ray is traced.** The first aims the sky's one ray, the second
    // pair's `x` picks which source it goes to, and the rest are drawn for their place alone: taking
    // one out shortens the sequence and moves every lamp draw below.
    const vec2 moonDraw[2]
        = vec2[2](vec2(randomNext(state), randomNext(state)), vec2(randomNext(state), randomNext(state)));
    const vec2 lampDraw = vec2(randomNext(state), randomNext(state));

    // **The sky's sources are weighed and drawn the way the lamps are.** What each would deliver
    // unshadowed is its weight — its cosine and its irradiance, which is everything about it that can
    // be known without tracing — one is drawn in proportion, one ray goes to it, and its share is
    // divided by the draw. In daylight the moons weigh nothing and the sun is always the draw; at
    // night the sun weighs nothing and the draw is between the moons; only the hour either side of
    // dusk spends one ray on two sources, and the accumulator carries the noise that buys.
    //
    // The cosine is taken against the source's direction *in air*, which is exact for the flat bed
    // this mostly lights: refraction at a level surface moves no flux across a horizontal patch, so
    // the irradiance on one below is the irradiance above times whatever the path took. A tilted
    // underwater surface would want the refracted direction and gets this one.
    //
    // **The disc is sampled for visibility and not for radiometry**, and that is the sharper of the
    // two estimators rather than a saving. Across the two degrees of the sun's shadow cone the cosine
    // varies by parts in a thousand, so drawing it as well would put variance into a term that has
    // none and leave the penumbra — the only part of the integral the cone is wide enough to matter
    // to — no better resolved for it. Masser subtends thirty-five times the sun's angle, so the cone
    // it is drawn from is that much wider and the penumbra under everything it lights that much
    // softer.
    //
    // **The moons are asked only where the eye can see the surface.** A bounce's far hit is an
    // indirect term nothing resolves on its own, so a moon reaching it through a shadow ray of its
    // own was the dimmest half of the dimmest thing in the frame.
    //
    // **A probability compared against the draw, and not a weight against a scaled draw.** The two
    // are the same until a source weighs nothing: the running share is then flat across it, and a
    // draw under the share before it has already picked. The draw is the moons' and the ray's pair
    // is the sun's, so every lamp draw below keeps its place.
    // **The three are named and not indexed**, for the reason `SkyChoice` gives: the pick is a value
    // the compiler cannot fold, and a local array read at one is a spill. The additions below are
    // the ones the loop made, in the order it made them, so the draw picks what it always picked.
    const bool lunar = HAS_MOONS && path == PATH_SEEN;
    const SkyChoice sun = skyChoiceAt(SKY_SOURCE_SUN, normal, side, transmission, sunUp());
    const SkyChoice masser = skyChoiceAt(SKY_SOURCE_MASSER, normal, side, transmission, lunar);
    const SkyChoice secunda = skyChoiceAt(SKY_SOURCE_SECUNDA, normal, side, transmission, lunar);

    const float total = sun.mWeight + masser.mWeight + secunda.mWeight;
    if (total > 0.0)
    {
        const float sunShare = sun.mWeight / total;
        const float moonShare = sunShare + masser.mWeight / total;

        SkyChoice picked = secunda;
        if (moonDraw[1].x < sunShare)
            picked = sun;
        else if (moonDraw[1].x < moonShare)
            picked = masser;

        const float chance = picked.mWeight / total;

        radiance += picked.mSky.mIrradiance * lightThroughWater(position, picked.mSky.mDirection, footprint)
            * (picked.mCosine * INV_PI * skyVisible(picked.mSky, position, sunDraw) / chance);
    }

    // A lamp loses nothing to the water, where the sun and the sky both lose the column above the
    // point: it is usually standing in the same water as what it lights, and the depth over the two
    // of them is not between them.
    // **Every lamp is weighed and one is kept, so the cost is one shadow ray however many there
    // are.** Walking them all spends a ray apiece, which is a per-pixel cost against a per-cell
    // question: a room with a dozen candles was a dozen shadow rays for every pixel of it.
    //
    // Resampled importance sampling. A candidate's weight is what it would deliver *unshadowed*,
    // which is everything about a lamp that can be known without tracing — its reach, its falloff
    // and the cosine — so the one that survives is nearly always the one that mattered. The
    // estimator then divides by the chance it was kept, which is what makes this unbiased rather
    // than merely cheap: the sum of every weight, over the weight of the one held.
    //
    // **With one lamp in the cell it is exactly the arithmetic that was here before**: the sum is
    // that lamp's weight, the ratio is one, and what is left is the term that was always there.
    Reservoir kept = noLamps();
    weighLamps(kept, state, position, normal, side, INV_PI, transmission);

    radiance += lampsThrough(kept, lampDraw);

    return radiance * rated;
}

/// What terminates a path: the cell's own ambient, dimmed by whatever stands over the point.
///
/// **A stand-in for every bounce that is not being traced**, which is what it always was — the
/// difference is that it is now one level down rather than added on top of the one that is. A
/// surface the eye can see gathers a real hemisphere; what *that* ray lands on gets this instead,
/// and the path stops there.
///
/// It stands in for light that arrived from above, so what it loses to water is the column straight
/// over the point — `daylightReaching`'s approximation, and the same one the bounce's own escape to
/// the sky uses.
///
/// @param reaching how much of the ambient the point can see, out of `ambientReaching`. It bites on
///        the whole of it and on both sides of a door: what a room changes is how far the ray looked
///        for the occluder, not whether the answer applies. One is what an asker with no hemisphere
///        to trace hands over.
vec3 pathEnd(vec3 position, float reaching)
{
    return frame.mAmbient * (daylightReaching(position) * reaching);
}

/// What a shading model made of a surface, in the terms a temporal upscaler demodulates by.
///
/// **Reported by whatever shaded the pixel rather than guessed after it.** Ray Reconstruction
/// separates a noisy pixel into a diffuse and a specular half using the albedos and the roughness it
/// is handed, so those three have to describe what this renderer actually did — and only the
/// function that did it knows. A constant roughness of one, a permanently zero specular albedo and
/// the *flat quad's* normal for water is a description of a renderer nobody wrote.
struct SurfaceResponse
{
    /// The normal the shading used, which for water is the wave's and not the plane's.
    vec3 mNormal;

    /// What the diffuse half is multiplied by, and nothing else: the surface's own albedo, with
    /// none of what the path took off it between here and the eye.
    vec3 mDiffuse;

    /// What the specular half is multiplied by — the surface's reflectance at this angle.
    vec3 mSpecular;

    /// Nought for a mirror and one for Lambert.
    float mRoughness;
};

/// A pixel with no surface behind it: the sky, or a ray that reached nothing.
SurfaceResponse noResponse()
{
    return SurfaceResponse(vec3(0.0), vec3(0.0), vec3(0.0), 1.0);
}

/// What `shadeSurface` does, said in those terms. Perfectly rough and perfectly diffuse, because
/// that is exactly what a Lambert model is — and until there is a material model saying otherwise,
/// it is the true answer rather than a stand-in for one.
SurfaceResponse lambertResponse(Surface surface)
{
    return SurfaceResponse(surface.mNormal, surface.mAlbedo, vec3(0.0), 1.0);
}

/// What an ordinary lit surface sends back along the ray that found it.
///
/// @param incoming what arrives from everything that is not a light: a gathered hemisphere at the
///        hit the eye found, and `pathEnd` at the hit that hemisphere found. **One statement of what
///        a diffuse surface does with light, used at both depths** — writing it twice is how the two
///        would come to disagree.
vec3 shadeSurface(Surface surface, vec3 incoming, uint seed, uint path)
{
    // The emissive colour joins the light rather than the albedo, which is where the original engine
    // puts it: it sums the term with the diffuse and ambient light and multiplies the whole by the
    // texture (`files/shaders/compatibility/objects.frag:232`). Added past the albedo instead, a
    // mushroom cap carrying half against its stalk's nothing comes out flat white.
    //
    // The emissive *map* is the other way round, and that is the engine's doing too
    // (`objects.frag:244`): added after the multiply, so it glows through whatever the surface is
    // made of rather than being tinted by it.
    return surface.mAlbedo
        * (incoming
            + gather(surface.mPosition, surface.mNormal, surface.mClosed ? surface.mNormal : surface.mGeometric,
                surface.mFootprint, surface.mTransmission, seed, path)
            + surface.mEmissiveColour * EMISSIVE_INTENSITY)
        + surface.mEmitted;
}

/// Which face of a surface a diffuse sample leaves by, and what the sample is then worth.
///
/// **A sheet has two hemispheres and one ray.** Its diffuse response is the near hemisphere whole
/// and the far one at `transmission`, so the far side is taken with probability
/// `transmission / (1 + transmission)` and either sample is weighed by `1 + transmission` — which
/// is exactly the two integrals' sum, and one ray however many faces there are. A solid never
/// draws: its weight is one and its near face is its own.
///
/// **A sign rather than an axis, because a face is two vectors and not one.** The hemisphere is
/// drawn about the shading normal and bounded by the triangle's plane, and the far face is the far
/// side of both — so what the caller needs back is the turn to apply to each.
///
/// @param draw one number in `[0, 1)`, read only where there is a far side to choose.
float sampledFace(float transmission, float draw, out float weight)
{
    weight = 1.0 + transmission;
    return transmission > 0.0 && draw * weight > 1.0 ? -1.0 : 1.0;
}

/// Whether a direction drawn about the shading normal has gone behind the face it left by.
///
/// **The lobe is the shading normal's, which is what a shading normal is for, and the triangle is
/// what bounds it.** On this content a normal leans past its own plane often enough to matter — four
/// hits in a hundred by more than sixty degrees — and a direction drawn past it sets off *into* the
/// surface. Morrowind builds rooms out of sheets with no thickness, so nothing stops such a ray:
/// it either leaves the building, where a room hands nothing back and the point goes dark, or it
/// lands on the far face of the wall and hands back light the point cannot see. Over a converged
/// frame of a room those rays are most of the mean escaped share and nearly all of the worst
/// pixels'.
///
/// **What a caller takes from such a direction is nothing, because nothing is the answer**: what it
/// points at is the inside of the surface. The cost is the part of the lobe that leans past the
/// plane, and that loss is the shading normal's own rather than one made here.
///
/// @param plane the surface's `Surface::mGeometric` — or nothing at all for a point in a medium,
///        which has no triangle to be behind.
/// @param face which side `sampledFace` drew, which turns the plane along with the normal.
bool behindTheFace(vec3 towards, vec3 plane, float face)
{
    return dot(plane, plane) > 0.0 && face * dot(towards, plane) <= 0.0;
}

/// How much of the ambient a surface can see, as one cosine-weighted sample of its own hemisphere.
///
/// **The same integral the bounce already samples, one level further down.** A ray the eye found
/// gathers a real hemisphere and is occluded by whatever it hits; what *that* ray landed on was
/// handed the whole ambient whatever stood over it, so a point in a hollow was lit as though nothing
/// stood over it. This is the missing half, and it is a visibility ray rather than a bounce: nothing
/// is shaded at the far end, only asked whether there is one.
///
/// **One sample, and it is binary — half a sample out of doors.** That is as noisy as a single
/// sample can be, and it multiplies a term already carried by one, so it rides the same filter. The
/// estimator stays unbiased at either rate, which a cheaper guess would not be. See
/// `AMBIENT_EXTERIOR_RATE`.
///
/// **A sheet asks both faces**, by `sampledFace`: a leaf in the open sees the sky over its back as
/// well, at `transmission` of what it sees over its front, so what reaches it runs to
/// `1 + transmission` of a solid's whole. The side is drawn after the direction and only where
/// there is one to draw, so a solid's sequence is what it was.
///
/// **A direction behind the surface's own triangle reaches nothing**, by `behindTheFace`: there is
/// no ambient inside a wall.
///
/// @param normal the hemisphere's axis — a surface's shading normal, or nothing at all for a froxel
///        of the air, which is `weighLamps`' contract and means the same thing here: the air has no
///        side to face away from, so what stands over it is asked over the whole sphere rather than
///        over a hemisphere.
/// @param plane the surface's own triangle, as `behindTheFace` takes it.
float ambientReaching(vec3 position, vec3 normal, vec3 plane, float transmission, uint seed)
{
    uint state = randomSeed(seed);
    const vec2 draw = vec2(randomNext(state), randomNext(state));

    float weight = 1.0;
    vec3 towards;

    if (dot(normal, normal) > 0.0)
    {
        const float face = sampledFace(transmission, transmission > 0.0 ? randomNext(state) : 0.0, weight);
        towards = cosineDirection(normal * face, draw);

        if (behindTheFace(towards, plane, face))
            return 0.0;
    }
    else
        towards = sphereDirection(draw);

    // **How far to look for the occluder is what a room changes first.** In a room the ambient is
    // the `AMBI` fill, which stands for the bounces the room itself makes — so a wall is not an
    // occluder of it, it is the thing making it, and a ray run to the walls comes back blocked
    // everywhere and takes the light out of every interior. What does occlude it is what stands
    // between a point and the room: the pillow over the sheet, the chest against the wall, the
    // underside of a table. A room keeps every sample too — `AMBIENT_EXTERIOR_RATE` says why.
    if (!skyLights())
        return weight * lightThrough(position, towards, ROOM_FILL_REACH);

    // Drawn last, so a solid's direction and a sheet's side are the numbers they were.
    if (randomNext(state) >= AMBIENT_EXTERIOR_RATE)
        return 0.0;

    return weight * lightThrough(position, towards, frame.mFar) / AMBIENT_EXTERIOR_RATE;
}

/// What a bounce brings back when it reaches nothing.
///
/// The glow and not the disc: the sun is already a term of its own in `gather`, and a bounce that
/// found it in the sky would be the same light counted twice.
///
/// **A room has nothing outside it, so a ray that got out of one brings back nothing.** The dome a
/// room draws is its fog colour standing in for the *picture* wherever a ray leaves the shell, and it
/// is far brighter than the room itself: lighting with it drew a bright band along the foot of every
/// wall. `mAmbientFromSky` is where a cell answers whether its sky is a light, and `ambientReaching`
/// reads the same field to decide how far to look for what blocks the fill.
///
/// Dimmed by the column of water over the point, on `daylightReaching`'s vertical approximation and
/// for its reason: this ray left for the sky and the sky is above, so what stands between them is the
/// depth. Without it a flooded floor reads brighter than the same floor seen from over the surface,
/// which is the disagreement M6 closed.
vec3 bounceEscape(vec3 position, vec3 towards, float weight)
{
    if (!skyLights())
        return vec3(0.0);

    return weight * skyGlow(towards) * daylightReaching(position);
}

/// What reaches a surface from everything that is not a light: one diffuse bounce.
///
/// **Traced only from the hit the eye found.** A shader with no recursion cannot bounce a bounce, and
/// it should not: what the second hit gathers is `pathEnd`, the flat ambient that stands in for the
/// rest of the path. That is also what keeps `shadeSurface` from calling itself — the water's
/// reflections already shade through it, and a bounce inside it would have no bottom.
///
/// A ray that finds nothing takes `bounceEscape`, which is what makes the sky an emitter rather than
/// a backdrop: outdoors it is by far the largest source in the scene, and a surface facing it should
/// be lit by it.
vec3 bounceLight(Surface surface, uvec2 pixel)
{
    // A sheet bounces off either face, and `SEED_SHEET_SIDE` says why the side is not drawn from
    // the pair the direction is.
    float weight;
    float side = 0.0;
    if (surface.mTransmission > 0.0)
    {
        uint state = randomSeed(pixelKey(pixel) + SEED_SHEET_SIDE);
        side = randomNext(state);
    }
    const float face = sampledFace(surface.mTransmission, side, weight);

    const vec3 towards = cosineDirection(surface.mNormal * face, unitPair(pixel, STREAM_BOUNCE));

    if (behindTheFace(towards, surface.mGeometric, face))
        return vec3(0.0);

    // **Far ground out of doors is handed the escape rather than asked whether it escaped**, which
    // is the same answer the miss below arrives at by tracing for it. `BOUNCE_REACH` says what that
    // costs and why the room is not in it.
    const vec3 fromEye = surface.mPosition - frame.mOrigin;
    if (skyLights() && surface.mGround && dot(fromEye, fromEye) > BOUNCE_REACH * BOUNCE_REACH)
        return bounceEscape(surface.mPosition, towards, weight);

    // **An inline query inside the closest-hit shader, and not a second launch-side trace.** The
    // one ray Shader Execution Reordering's sources point at is this one, and sorting for it was
    // measured before the trace was split: 20 percent slower out of doors and 30 in a room, because
    // a bounce indoors is short and lands on the same few surfaces, so there is no coherence left to
    // recover. `.notes/rtx/gpu-performance.md` holds every reading since.
    const Surface hit
        = trace(surface.mPosition, towards, SHADOW_BIAS, surface.mFootprint, BOUNCE_SPREAD, solidMask(frame.mRayMask));

    if (!hit.mHit)
        return bounceEscape(surface.mPosition, towards, weight);

    const float reaching = ambientReaching(
        hit.mPosition, hit.mNormal, hit.mGeometric, hit.mTransmission, pixelKey(pixel) + SEED_AMBIENT_REACHING);

    // **Its glow is counted here, because this is the only path it takes.** Nothing gives a glowing
    // surface a lamp of its own — `EMISSIVE_INTENSITY` says what measuring that showed — so a ray
    // that lands on a mushroom cap is what carries the cap's glow back to whatever sent it.
    return weight
        * shadeSurface(hit, pathEnd(hit.mPosition, reaching), pixelKey(pixel) + SEED_LAMPS_BOUNCE, PATH_INDIRECT);
}

/// What a solid the eye found is: its direct light, the one bounce it gathers, and what it is in the
/// upscaler's terms.
///
/// **One statement of what a ground pixel is, used twice** — for the hit itself, and for the bed
/// under a waterline pixel, which is that ground and has to be shaded exactly as it. Written twice
/// is how the two would come to disagree.
void shadeSolid(Surface hit, uvec2 pixel, out vec3 direct, out vec3 bounce, out SurfaceResponse response)
{
    direct = shadeSurface(hit, vec3(0.0), pixelKey(pixel) + SEED_LAMPS_EYE, PATH_SEEN);
    bounce = bounceLight(hit, pixel);
    response = lambertResponse(hit);
}

#endif
