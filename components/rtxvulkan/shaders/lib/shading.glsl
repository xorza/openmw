// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_SHADING_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_SHADING_GLSL

// What an ordinary lit surface does with light: the direct sources it can ask about, the
// one bounce it traces for everything else, and the terms an upscaler demodulates by.

#include "scene.h"
#include "bindings.glsl"
#include "lights.glsl"
#include "random.glsl"
#include "sky.glsl"
#include "traversal.glsl"
#include "underwater.glsl"

/// The *direct* light arriving at a point and turning back out of it, per unit albedo.
///
/// **Sources that can be asked where they are, and nothing else.** The sun and the lamps are each a
/// known direction and a shadow ray; everything that arrives by having bounced off something is
/// `bounceLight`'s, and adding a fill here as well would count that half twice.
///
/// One shadow ray per light that could reach at all, and none for a light the surface faces away
/// from — the two tests before it are what keep a cell's worth of lamps affordable.
///
/// @param footprint how wide the cone that found this point had grown, which is the scale the
///        caustics are allowed to resolve waves at.
/// @param seed which draw sequence the lamp reservoir steps. **One per depth of the path**, because
///        a bounce shades a second surface and two reservoirs stepping one sequence would keep
///        correlated lamps at both ends of it.
vec3 gather(vec3 position, vec3 normal, float footprint, uint seed)
{
    vec3 radiance = vec3(0.0);

    uint state = randomSeed(seed);

    // **Where on a source a shadow ray leaves from is drawn before anything is weighed**, so that it
    // does not depend on how many lamps the cell happened to hold: the two pairs sit at a fixed
    // place in the sequence and the reservoir's own draws follow them. Otherwise a lamp arriving in
    // the next cell along would move the penumbra of the one already there.
    const vec2 sunDraw = vec2(randomNext(state), randomNext(state));
    const vec2 lampDraw = vec2(randomNext(state), randomNext(state));
    const vec2 moonDraw[2]
        = vec2[2](vec2(randomNext(state), randomNext(state)), vec2(randomNext(state), randomNext(state)));

    // **The cloud deck stands over the sun and the moons alike**, and it is the one occluder no ray
    // finds: the clouds are not in the acceleration structure and never will be, so what a light
    // above a point has to cross is asked of the sheet directly. `cloudShadow` says why it reads a
    // flat layer where the eye is given the mesh's bowl.
    //
    // The sun, which is one direction everywhere and needs none of the machinery below: no falloff,
    // no reach, and a shadow ray that runs until it leaves the world rather than until it arrives.
    //
    // The cosine is taken against the sun's direction *in air*, which is exact for the flat bed this
    // mostly lights: refraction at a level surface moves no flux across a horizontal patch, so the
    // irradiance on one below is the irradiance above times whatever the path took. A tilted
    // underwater surface would want the refracted direction and gets this one.
    //
    // **The disc is sampled for visibility and not for radiometry**, and that is the sharper of the
    // two estimators rather than a saving. Across half a degree the cosine varies by parts in a
    // million, so drawing it as well would put variance into a term that has none and leave the
    // penumbra — the only part of the integral the disc is wide enough to matter to — no better
    // resolved for it.
    const float sunCosine = dot(normal, frame.mSunPosition);
    if (sunCosine > 0.0 && frame.mSunIrradiance != vec3(0.0))
    {
        const float through
            = lightThrough(position, coneDirection(frame.mSunPosition, sin(SUN_ANGULAR_RADIUS), sunDraw), frame.mFar);

        radiance += frame.mSunIrradiance * lightThroughWater(position, frame.mSunPosition, footprint)
            * (sunCosine * INV_PI * through * cloudShadow(position, frame.mSunPosition));
    }

    // **The moons, which is the whole of what lights a night out of doors.** The same estimator as
    // the sun and for the same reasons, with two differences that are both the disc's size: Masser
    // subtends thirty-five times the sun's angle, so the cone it is drawn from is that much wider
    // and the penumbra under everything it lights is that much softer.
    //
    // **A ray apiece and the alpha decides whether it is spent.** The game fades both moons out over
    // the hours around dawn, so a daylit frame reaches `mIrradiance` of nothing and traces neither.
    for (uint moon = 0u; moon < 2u; ++moon)
    {
        const float moonCosine = dot(normal, frame.mMoons[moon].mDirection);
        if (moonCosine <= 0.0 || frame.mMoons[moon].mIrradiance == vec3(0.0))
            continue;

        const vec3 toward
            = coneDirection(frame.mMoons[moon].mDirection, sin(frame.mMoons[moon].mAngularRadius), moonDraw[moon]);

        radiance += frame.mMoons[moon].mIrradiance
            * lightThroughWater(position, frame.mMoons[moon].mDirection, footprint)
            * (moonCosine * INV_PI * lightThrough(position, toward, frame.mFar)
                * cloudShadow(position, frame.mMoons[moon].mDirection));
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
    weighLamps(kept, state, position, normal, INV_PI);

    radiance += lampsThrough(kept, lampDraw);

    return radiance;
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
/// @param reaching how much of the sky the point can see, out of `skyReaching`. It bites only on
///        the share of the ambient that *is* the sky — `mAmbientFromSky` — because a room's `AMBI`
///        is a fill standing for the bounces that room makes and reaches a corner as much as
///        anywhere else. One is what an asker with no hemisphere to trace hands over.
vec3 pathEnd(vec3 position, float reaching)
{
    return frame.mAmbient * (daylightReaching(position) * (1.0 - frame.mAmbientFromSky * (1.0 - reaching)));
}

/// What a shading model made of a surface, in the terms a temporal upscaler demodulates by.
///
/// **Reported by whatever shaded the pixel rather than guessed after it.** Ray Reconstruction
/// separates a noisy pixel into a diffuse and a specular half using the albedos and the roughness it
/// is handed, so those three have to describe what this renderer actually did — and only the
/// function that did it knows. The frame used to answer with a constant roughness of one, a
/// permanently zero specular albedo and the *flat quad's* normal for water, which is a description
/// of a renderer nobody wrote.
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
vec3 shadeSurface(Surface surface, vec3 incoming, uint seed)
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
        * (incoming + gather(surface.mPosition, surface.mNormal, surface.mFootprint, seed)
            + surface.mEmissiveColour * EMISSIVE_INTENSITY)
        + surface.mEmitted;
}

/// How fast a bounce ray's cone widens, against a primary ray's.
///
/// A diffuse bounce spreads over the whole hemisphere, and what the indirect term wants from a
/// texture is its *average* rather than any texel of it — so the cone is opened to about a radian,
/// which reads the coarse mips a bounce should see without collapsing every one to the top level.
const float BOUNCE_SPREAD = 1.0;

/// How much of the sky a surface can see, as one cosine-weighted sample of its own hemisphere.
///
/// **The same integral the bounce already samples, one level further down.** A ray the eye found
/// gathers a real hemisphere and is occluded by whatever it hits; what *that* ray landed on was
/// handed the open sky whatever stood over it, so a point in an exterior hollow was lit as though
/// the sky reached it. This is the missing half, and it is a visibility ray rather than a bounce:
/// nothing is shaded at the far end, only asked whether there is one.
///
/// **One sample, and it is binary.** That is as noisy as a single sample can be, and it multiplies a
/// term already carried by one — so it rides the same filter, and the estimator is unbiased where a
/// cheaper guess would not be.
float skyReaching(vec3 position, vec3 normal, uint seed)
{
    // **A room traces nothing**, which is most of what this costs: its ambient is a fill rather than
    // the sky, so `pathEnd` would throw the answer away and the ray is not spent to get it.
    if (!(frame.mAmbientFromSky > 0.0))
        return 1.0;

    uint state = randomSeed(seed);
    const vec2 draw = vec2(randomNext(state), randomNext(state));

    return lightThrough(position, cosineDirection(normal, draw), frame.mFar);
}

/// What reaches a surface from everything that is not a light: one diffuse bounce.
///
/// **Traced only from the hit the eye found.** A shader with no recursion cannot bounce a bounce, and
/// it should not: what the second hit gathers is `pathEnd`, the flat ambient that stands in for the
/// rest of the path. That is also what keeps `shadeSurface` from calling itself — the water's
/// reflections already shade through it, and a bounce inside it would have no bottom.
///
/// A miss returns the sky, which is what makes the sky an emitter rather than a backdrop: outdoors
/// it is by far the largest source in the scene, and a surface facing it should be lit by it.
vec3 bounceLight(Surface surface, uvec2 pixel)
{
    const vec3 towards = cosineDirection(surface.mNormal, unitPair(pixel, STREAM_BOUNCE));
    const Surface hit
        = trace(surface.mPosition, towards, SHADOW_BIAS, surface.mFootprint, BOUNCE_SPREAD, MASK_SOLID);

    // The glow and not the disc: the sun is already a term of its own in `gather`, and a bounce
    // that found it in the sky would be the same light counted twice.
    //
    // Dimmed by the column of water over the point, on `daylightReaching`'s vertical approximation
    // and for its reason: this ray left for the sky and the sky is above, so what stands between
    // them is the depth. Without it a flooded floor reads brighter than the same floor seen from
    // over the surface, which is the disagreement M6 closed.
    if (!hit.mHit)
        return skyGlow(towards) * daylightReaching(surface.mPosition);

    const float reaching = skyReaching(hit.mPosition, hit.mNormal, pixelKey(pixel) + SEED_SKY_REACHING);

    return shadeSurface(hit, pathEnd(hit.mPosition, reaching), pixelKey(pixel) + SEED_LAMPS_BOUNCE);
}

#endif
