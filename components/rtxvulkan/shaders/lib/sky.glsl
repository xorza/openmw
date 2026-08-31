// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_SKY_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_SKY_GLSL

// What a ray that reached nothing comes back with: the dome, the deck over it, the sheet
// behind it, and the three discs in between.
//
// **The sky is a light source and not a backdrop**, so this is split in two: `skyGlow` is
// what a bounce may gather, and `skyRadiance` is what an eye may see. The difference is the
// sun's disc, which `gather` already asks about directly.

#include "colour.h"
#include "scene.h"
#include "visibility.h"
#include "bindings.glsl"
#include "variants.glsl"

/// The sky's own glow along a direction, with nothing drawn in it.
///
/// **This is the sky as a source of light**, which is not the sky as a thing to look at, and the
/// two differ both ways. What a bounce gathers must leave out the sun's disc: `gather` asks the sun
/// directly, so a hemisphere that also picked it up out of the sky would count it twice — and a
/// bounce cone a radian wide would pick it up over a quarter of the sky at that. It must carry
/// `mSkyFill`, which is light the weather has and the sky is not painted with. And it takes the
/// night's sheets as one mean rather than where they actually are, because a cosine lobe is a
/// hemisphere and a ray is a direction — `NightSky::mGlow` carries that argument.
vec3 skyGlow(vec3 direction)
{
    return skyGradient(frame.mSkyHorizon, frame.mSkyZenith, direction) + frame.mStars.mGlow + frame.mSkyFill;
}

/// Where a point on the layer sits on the sheet, in texture coordinates.
///
/// **Addressed from the world and not from the eye**, which is the whole of what lets the deck cast:
/// a sheet laid out from where a ray happened to be looking travels with the camera, and a shadow
/// off one would travel with it too rather than lie under the cloud that made it.
///
/// The turn is about the world's own origin, so that a ray from an eye and a ray from a shading
/// point reach one answer. Nothing this renderer draws can see where that centre is: the four
/// weathers that drive a storm are ash, blight, snow and blizzard, and not one of them reaches a
/// cloud sheet the archives hold.
vec2 cloudUvAt(vec2 crossing, vec2 bearing)
{
    const vec2 along
        = vec2(crossing.x * bearing.x - crossing.y * bearing.y, crossing.x * bearing.y + crossing.y * bearing.x);

    return along * frame.mClouds.mPerTile + vec2(0.0, frame.mClouds.mScroll);
}

/// The sheet where a crossing lands on it, across whatever transition the weather is part way
/// through.
///
/// **One reading, because two things ask for it**: what the eye finds in the deck, and what the deck
/// leaves of a light standing over a shading point. The two used to sample the same sheet with the
/// same blend written out twice, which is what the reference had already seen drift.
///
/// **The top mip and no cone.** A deck seen edge-on wants a level off the ray's gradient, and the
/// gradient is what the hardware works out for itself from neighbouring lanes, which a ray tracer
/// does not have. The engine's own fade is what stands in: the band where the stretch would alias is
/// the band it takes the deck out over.
vec4 cloudSheetAt(vec2 crossing)
{
    const vec4 near
        = textureLod(textures[nonuniformEXT(frame.mClouds.mTexture)], cloudUvAt(crossing, frame.mClouds.mBearing), 0.0);
    const vec4 far = frame.mClouds.mNext == NO_TEXTURE
        ? near
        : textureLod(
              textures[nonuniformEXT(frame.mClouds.mNext)], cloudUvAt(crossing, frame.mClouds.mNextBearing), 0.0);

    return mix(near, far, frame.mClouds.mBlend);
}

/// How high the layer stands over a point, or nothing at all where it stands under one.
///
/// **A world height rather than one over the eye**, which is what a shadow needs: a layer that rose
/// with the camera would cast a shadow that moved with it. What still follows the eye is the deck's
/// *extent*, because the fade rings are the mesh's own and are measured from there.
float cloudLayerOver(vec3 at)
{
    return max(frame.mClouds.mAltitude - at.z, 0.0);
}

/// What a ray that reached nothing finds in the cloud deck, and how much of the sky it hides.
///
/// **The deck is where the ray crosses a layer over a curved world**, which is the thing the game's
/// cloud mesh is a stand-in for and the shape `CloudShell` reads back out of it. Where it hangs is
/// all Morrowind's: the texture is the weather's own, the scroll and the bearing are what the engine
/// turns its mesh by, the blend across a transition is the same factor, and the layer's height and
/// curvature and where it fades out are its mesh's.
///
/// **Alpha is coverage and the sheet's own paint is shape**, which is where this parts company with
/// the engine. Its deck is an unlit alpha-tracking material that owes nothing to where the sun is,
/// painted with its own sheet times a lifted fog; this one takes the alpha for how much sky a cloud
/// replaces and the paint for how thick it is, and `Rtx::deckLight` says what a cloud that thick
/// radiates. A photograph of a 2002 sky is a shape and not a colour.
///
/// @param origin where the ray started, which is what the sheet is laid out from.
/// @param covered how much of what lies behind the deck it hides, which is what puts the stars out.
vec3 cloudDeck(vec3 origin, vec3 direction, out float covered)
{
    covered = 0.0;
    if (!(frame.mClouds.mOpacity > 0.0) || frame.mClouds.mTexture == NO_TEXTURE || direction.z <= 0.0)
        return vec3(0.0);

    // A ray that starts over the layer finds no deck, which is what an eye above the clouds sees.
    const float height = cloudLayerOver(origin);
    if (height <= 0.0)
        return vec3(0.0);

    const vec2 plane = direction.xy / direction.z;

    // **How far down the layer has fallen where this ray meets it.** A flat one is met at `xy / z`
    // and a curved one nearer, by the height of its crossing: solving `r = t (h - k r²)` gives
    // `h · 2 / (1 + sqrt(1 + 4kh t²))`, which is the conjugate of `(sqrt(1 + 4kh t²) - 1) / 2kt` and
    // is written that way because the other form divides by nothing on a ray straight up — and by a
    // curvature of nothing on the flat layer a replaced mesh may be.
    const float fallen = 2.0 / (1.0 + sqrt(1.0 + 4.0 * frame.mClouds.mCurvature * dot(plane, plane)));

    const vec2 offset = plane * (height * fallen);

    // **How far out the ray met the layer, which is what the engine's fade is written in.** It paints
    // the outermost ring of its mesh at nothing and the one inside it at a quarter, and a triangle
    // between two rings interpolates that linearly in position — so the fade is a straight line in
    // this radius, and the three radii it turns on came off the same mesh. Measured from the eye,
    // because the rings are the mesh's own and the mesh is centred there.
    const vec3 rings = frame.mClouds.mRings;
    const float reach = length(offset * frame.mClouds.mPerTile);
    if (reach >= rings.z)
        return vec3(0.0);

    // Two clamped ramps nested rather than a pair of branches: the inner one carries the deck from
    // whole to a quarter and the outer takes what is left of it to nothing, and each is already at
    // its own end wherever the other is running. A band the rule left empty divides by nothing.
    const float inner = clamp((reach - rings.x) / max(rings.y - rings.x, 1.0e-6), 0.0, 1.0);
    const float outer = clamp((reach - rings.y) / max(rings.z - rings.y, 1.0e-6), 0.0, 1.0);
    const float reaches = mix(mix(1.0, CLOUD_RING_ALPHA, inner), 0.0, outer);

    const vec4 cloud = cloudSheetAt(origin.xy + offset);

    covered = cloud.a * reaches * frame.mClouds.mOpacity;

    // **The sheet says how thick the cloud is and the light says what that is worth.** A luminance
    // and not the three channels, because a sheet's own hue is the 2002 sky behind it and carrying
    // that would paint a day's light into the deck twice. `CLOUD_THICKNESS_MAX` is where the ratio
    // reaches a cloud in full sun; a sheet nothing could average has no ratio to take, and reads as
    // the average cloud it could not measure.
    const float thickness = frame.mClouds.mMean > 0.0
        ? clamp(dot(cloud.rgb, LUMINANCE_WEIGHTS) / (frame.mClouds.mMean * CLOUD_THICKNESS_MAX), 0.0, 1.0)
        : 1.0 / CLOUD_THICKNESS_MAX;

    // **Thick where it is lit and thin where it is not.** A cloud's own body is what keeps the sun
    // and the moons off its base, so the dense parts of the sheet show them and the wisps show only
    // the sky — which is the difference between the two colours `Rtx::deckLight` handed over.
    const vec3 radiance = mix(frame.mClouds.mShadowed, frame.mClouds.mLit, thickness);

    // **And the colour crosses to the air over the same stretch**, which is the second thing the
    // engine does with that vertex alpha: `paintClouds` mixes the deck toward the fog by it, so the
    // last rings of cloud are the horizon's own colour rather than a thin wash of a distant one.
    return mix(frame.mSkyHorizon, radiance, reaches) * covered;
}

/// What the cloud layer leaves of a light standing over `position`.
///
/// **A flat layer, where the eye is given the mesh's bowl, and the two agree where anyone could
/// check.** Morrowind's cap is a strong compression — it lets the deck reach 4.7 degrees above the
/// horizon where a flat layer stops at 15.9 — and it is centred on the viewer by construction, so a
/// world-anchored copy of it does not exist. A shadow has no viewer to be centred on, so it takes
/// the honest crossing. Straight overhead the bowl and the plane meet exactly and it is the same
/// texel; at 45 degrees they are 5% apart and at 14 degrees half again, which is a cloud low in the
/// sky whose shadow is kilometres away and out of the frame it would have to be compared in.
///
/// **Beer-Lambert and not a crossfade**, which the reference measured: mixing toward a floor by
/// coverage saturates, and deepening that mix enough for a cirrus sky to cast anything pinned 48.5%
/// of the sheet at one flat value. An exponential never flattens, so the pattern on the ground stays
/// the pattern in the sky.
///
/// `CLOUD_SHADOW_DEPTH` says why it is the alpha *over the sheet's own mean* that darkens.
float cloudShadow(vec3 position, vec3 towards)
{
    if (!(frame.mClouds.mOpacity > 0.0) || frame.mClouds.mTexture == NO_TEXTURE || towards.z <= 0.0)
        return 1.0;

    const float height = cloudLayerOver(position);
    if (height <= 0.0)
        return 1.0;

    const float alpha = cloudSheetAt(position.xy + towards.xy * (height / towards.z)).a;

    return exp(-CLOUD_SHADOW_DEPTH * max(alpha - frame.mClouds.mCover, 0.0) * frame.mClouds.mOpacity);
}

/// What the nebulae and the constellations send back along a ray.
///
/// **The same disc a moon is, and drawn by the same arithmetic.** Each is a sheet laid once across a
/// patch of the sky, so a direction, a size and two axes are the whole of it — no phase, no shading
/// law, nothing lit. They are additive over the sky because that is the material the engine gives
/// them: unlit, alpha-tracking, nothing behind them to be occluded.
///
/// **Most of a Morrowind night's colour is here** rather than in the stars. Two of the three nebulae
/// reach past a radian, so what they do is tint half the sky at a time — which is why a renderer
/// drawing the star field alone puts stars on black.
vec3 skyPatches(vec3 direction)
{
    vec3 painted = vec3(0.0);

    // `patch` is a reserved word in GLSL, which is why this is not called one.
    for (uint layer = 0u; layer < SKY_PATCH_COUNT; ++layer)
    {
        const SkyPatch sheet = frame.mSkyPatches[layer];

        // The hemisphere test is not optional: the offsets below are the same for a direction and
        // its opposite, so without it every ray pointing away lands in the middle of the face.
        if (sheet.mTexture == NO_TEXTURE || dot(direction, sheet.mDirection) <= 0.0)
            continue;

        // Where across the face, in units of its radius — the moons' own mapping, for the reason
        // they share.
        const vec2 at
            = vec2(dot(direction, sheet.mRight), dot(direction, sheet.mUp)) / max(sheet.mLimb, 1.0e-4);
        if (dot(at, at) >= 1.0)
            continue;

        const vec4 texel = textureLod(textures[nonuniformEXT(sheet.mTexture)], 0.5 + 0.5 * at, 0.0);
        painted += texel.rgb * (texel.a * NEBULA_RADIANCE);
    }

    return painted;
}

/// What a moon's lit face sends back along a ray, and how much of the sky it stands in front of.
///
/// **The disc is the sphere seen flat**, so the surface normal is recovered rather than stored: a
/// point at `(x, y)` across the face, in units of its own radius, sits at a height of
/// `sqrt(1 - x² - y²)` on a unit sphere. One square root buys a terminator that curves the way a
/// real one does and moves continuously, where a selector between eight painted phases would step.
///
/// **The lit share is the game's and the direction it faces is the sky's.** Morrowind advances a
/// phase on a three-day clock that owes nothing to where its sun actually is, so the share has to
/// come from `mPhaseAngle`; but a crescent that did not point at the sun would read as a mistake, so
/// the terminator is turned toward it. The two answers are independent and neither can be dropped.
///
/// @param covered how much of what lies behind the moon it hides — the star sheet, the painted
///        patches, the sun, and the other moon.
vec3 moonFace(MoonDisc moon, vec3 direction, float blur, out float covered)
{
    covered = 0.0;

    // A moon that is down, or one the far side of the sky. **The hemisphere test is not optional**:
    // the offsets below are the same for a direction and its opposite, so without it every ray
    // pointing away from a moon would land in the middle of its face.
    if (moon.mAlpha <= 0.0 || dot(direction, moon.mDirection) <= 0.0)
        return vec3(0.0);

    // Where across the face, in units of its radius: dividing by the limb puts it at one and makes
    // the cone test a comparison this needed anyway.
    const vec2 at = vec2(dot(direction, moon.mRight), dot(direction, moon.mUp)) / moon.mLimb;
    const float across = length(at);

    // The pixel's own spread in the same units, so the silhouette is antialiased rather than
    // stepped. A moon is degrees wide and a pixel a thousandth of one, so this is a hair either
    // side of the limb and nothing anywhere else.
    const float fade = max(blur / moon.mLimb, 1.0e-5);
    covered = (1.0 - smoothstep(1.0 - fade, 1.0 + fade, across)) * moon.mAlpha;
    if (covered <= 0.0)
        return vec3(0.0);

    // Clamped into the disc before the height is taken, so the band the antialiasing covers reads
    // the limb's own shading instead of the square root of a negative number.
    const vec2 face = at / max(across, 1.0);
    const vec3 normal = vec3(face, sqrt(max(1.0 - dot(face, face), 0.0)));

    const vec2 toward = vec2(dot(frame.mSunPosition, moon.mRight), dot(frame.mSunPosition, moon.mUp));
    const float turn = dot(toward, toward) > 0.0 ? atan(toward.y, toward.x) : 0.0;
    const vec3 light
        = vec3(sin(moon.mPhaseAngle) * cos(turn), sin(moon.mPhaseAngle) * sin(turn), cos(moon.mPhaseAngle));

    const float incidence = max(dot(normal, light), 0.0);
    const float emission = max(normal.z, 1.0e-4);

    // **McEwen's lunar-Lambert, because a Lambertian sphere does not look like a moon.** A rough
    // dusty surface scatters back the way the light came, which is why the real one reads as a flat
    // disc rather than a lit ball, and Lommel-Seeliger's `mu0 / (mu0 + mu)` is that in one divide.
    // Alone it puts the sunward limb at exactly twice the middle at every phase but full — its
    // emission cosine goes to zero there while the incidence cosine does not — so it is blended
    // toward a Lambertian term, whose cosine does vanish. The polynomial is McEwen's own, in the
    // phase angle in degrees.
    const float phase = degrees(moon.mPhaseAngle);
    const float lunar
        = clamp(1.0 - 0.019 * phase + 0.000242 * phase * phase - 1.46e-6 * phase * phase * phase, 0.0, 1.0);
    const float shade = lunar * 2.0 * incidence / (incidence + emission) + (1.0 - lunar) * incidence;

    // **The portrait where there is one, its mean where there is not.** `mColour` carries the mean
    // so the two paths land at the same brightness and only the detail differs — the file decides
    // the maria and the silhouette, `MOON_RADIANCE` decides how bright a full moon is, and neither
    // is scaling the other.
    vec3 base = moon.mColour;
    if (moon.mFace != NO_TEXTURE)
    {
        // `u` runs with the face's right and `v` against its up, which is the Y-down convention the
        // quad the game draws is authored in.
        const vec2 uv = vec2(0.5 + 0.5 * face.x, 0.5 - 0.5 * face.y);
        const vec4 painted = textureLod(textures[nonuniformEXT(moon.mFace)], uv, 0.0);

        // **Multiplied by its own alpha, which the file does not do for us.** Past the edge of the
        // painted disc the colour climbs back toward the middle of its range, so sampling the colour
        // and dropping the alpha draws a bright ring around every moon; multiplying removes it and
        // hands over the limb's own antialiasing at the same time.
        base = painted.rgb * painted.a;
    }

    // **Dimmed by the air it is seen through**, which is what takes a moon out near the horizon
    // here — the engine switches one off under `Fade_End_Angle` instead. `mThroughAir` is per
    // channel, so a low moon reddens as it goes rather than merely fading.
    return base * moon.mThroughAir * (MOON_RADIANCE * shade * covered);
}

/// The radiance a ray that hit nothing comes back with, for a ray being looked along.
///
/// **The sun is drawn here rather than answered by a lobe on each surface that could reflect it.**
/// A glint is what a mirror does when there is something to see, so the water needs no highlight
/// model of its own: it already traces a reflection ray, and this is what that ray finds. Anything
/// else reflective gets the same sun for nothing, and there is one place where the sun's size lives.
///
/// **Drawn, and lighting nothing.** `gather` asks the sun directly, so a path that also gathered the
/// sky as a source would count it twice — this is the sky as a thing to look *at*.
///
/// The disc is widened by the ray's own spread and dimmed by exactly the widening, so what changes
/// is where the light is and never how much. Two things widen it and both have to. **The pixel**,
/// because a sun smaller than the pixel that found it has to be averaged over the pixel rather than
/// hit or missed — sampled instead, it is a one-pixel speck that crawls as the camera moves. And
/// **the slopes the cone could not resolve**, because water too fine to draw is not flat: what those
/// slopes do to a reflected sun is spread it, and that spreading *is* the glitter path. A mirror
/// shows one hard dot; a mile of ruffled water shows a shimmering road to the horizon. Cox and Munk
/// measured sea roughness by photographing exactly this in 1954.
///
/// **Everything but the star field, which every caller draws for itself as `starField(...) * shown`.**
/// A point source is what a temporal upscaler removes, so along a ray the eye is looking down the
/// field is drawn by the display pass at the resolution the frame is shown at — and a pass that late
/// cannot see the order the layers here were composited in. So the order reports what it left: an
/// opaque moon leaves none of the field, a deck leaves what it does not cover, and the dome and the
/// sun leave all of it because both are added rather than composited.
///
/// @param origin where the ray started, which the deck is laid out from.
/// @param blur how far this ray's cone has spread from its axis, in radians.
/// @param shown how much of the star field is still in front of what this returns, from none to all.
vec3 skyRadiance(vec3 origin, vec3 direction, float blur, out float shown)
{
    shown = 1.0;

    // **Everything on or beyond the celestial sphere first, which is what a moon stands in front
    // of.** The painted patches are on that sphere and the sun is nearer, but both are further off
    // than a moon and neither is ever between two of them, so one term carries the pair. The star
    // field belongs here too and is the one layer drawn outside this, which `shown` is what for.
    vec3 colour = frame.mStars.mFade > 0.0 ? frame.mStars.mFade * skyPatches(direction) : vec3(0.0);

    // The chord across the disc rather than the cosine of its angle. Both answer "is this direction
    // inside it", and at half a degree the cosine is 0.999988 — five of a float's seven digits spent
    // before the question is asked. `|a - b|` is `2 sin(theta / 2)` for unit vectors, which loses
    // nothing, and it is the same quantity the cap's solid angle is built from: `pi * chord^2`.
    //
    // **Drawn on exactly the frames the sun lights anything**, because they are one fact: the
    // irradiance is nought whenever the sun is not over the horizon, and fades to it across dusk. A
    // second field saying whether to draw the disc is what once let a sun shadow out of an empty
    // sky, and there is no longer one to disagree with.
    const float edge = 2.0 * sin(0.5 * (SUN_ANGULAR_RADIUS + blur));
    if (HAS_SUN && frame.mSunIrradiance != vec3(0.0) && length(direction - frame.mSunPosition) < edge)
    {
        // **The sun's radiance is five orders of magnitude above the sky's** and this does not
        // pretend otherwise, so it saturates until there is an exposure stage to bring it down.
        // That is a fact about the sun rather than a choice made here: a photograph exposed for a
        // landscape shows a white disc, and a glitter road really is a field of blown-out sparks.
        // **How bright from the light, what colour from the disc**, which is why the brightest
        // channel and not the whole vector. The two are one quantity in the world and two in the
        // content files, and the disc's is the one that is about the sun: taking the hue off the
        // irradiance draws a blue sun through every dawn, because that ramp is still crossing to a
        // night colour that belongs to the sky rather than to anything the sun did. Reading the
        // peak leaves a white disc at white and lets a weather's sunset tint both redden and dim
        // it, which is what air does to a sun on the horizon — it takes the blue out rather than
        // putting red in.
        // Capped for the sake of what holds a history of it rather than for the picture, which
        // cannot tell this from the five figures the division gives. `MAX_SUN_RADIANCE` says why.
        const float radiance
            = min(brightest(frame.mSunIrradiance) / (0.5 * TAU * edge * edge), MAX_SUN_RADIANCE);

        colour += radiance * frame.mSunDiscColour;
    }

    // **Each moon takes its share of all of it, in the order the engine draws them.**
    // `SkyManager::create` builds the sky as atmosphere, night sky, sun, Masser, Secunda, cloud —
    // and `paintMoon` writes `color.a = maskAlpha` under a `(ONE, ONE_MINUS_SRC_ALPHA)` blend, so an
    // opaque moon replaces whatever the star sheet and the sun put behind it. `moonFace` already
    // hands back its radiance premultiplied by that coverage, which is the same form.
    //
    // **This is also the whole of an eclipse**, and of one moon in front of the other: Masser is
    // nineteen degrees across against the sun's half a degree, so on the rare crossing it is total.
    if (HAS_MOONS)
        for (uint moon = 0u; moon < 2u; ++moon)
        {
            float covered;
            const vec3 face = moonFace(frame.mMoons[moon], direction, blur, covered);
            colour = colour * (1.0 - covered) + face;
            shown *= 1.0 - covered;
        }

    // **The dome last and added rather than composited under, because it is the air in front of
    // every one of them.** A moon is seen through the same sky the eye is looking at — the engine
    // reaches the same picture from the other end, by having a moon re-add the atmosphere colour it
    // just replaced.
    //
    // **The gradient and not `skyGlow`**, which is the one place the two part company: the fill is
    // light the weather says a night has and Morrowind draws nowhere, so an eye must not find it.
    colour += skyGradient(frame.mSkyHorizon, frame.mSkyZenith, direction);

    // Last, and over everything: the deck is nearer than any of it.
    float covered;
    const vec3 clouds = cloudDeck(origin, direction, covered);
    shown *= 1.0 - covered;

    return colour * (1.0 - covered) + clouds;
}

#endif
