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

/// The sky's own glow along a direction, with nothing drawn in it.
///
/// **This is the sky as a source of light**, which is not the sky as a thing to look at, and the
/// two differ both ways. What a bounce gathers must leave out the sun's disc: `gather` asks the sun
/// directly, so a hemisphere that also picked it up out of the sky would count it twice — and a
/// bounce cone a radian wide would pick it up over a quarter of the sky at that. It must carry
/// `mSkyFill`, which is light the weather has and the sky is not painted with. And it takes the
/// night's sheets as one mean rather than where they actually are, because a star field sampled a
/// ray at a time is a firefly — `NightSky::mGlow` carries that argument.
vec3 skyGlow(vec3 direction)
{
    return skyGradient(frame.mSkyHorizon, frame.mSkyZenith, direction) + frame.mStars.mGlow + frame.mSkyFill;
}

/// What a ray that reached nothing finds in the cloud deck, and how much of the sky it hides.
///
/// **The deck is where the ray crosses a layer over a curved world**, which is the thing the game's
/// cloud mesh is a stand-in for and the shape `CloudShell` reads back out of it. Everything here is
/// Morrowind's: the texture is the weather's own, the scroll and the bearing are what the engine
/// turns its mesh by, the blend across a transition is the same factor, the layer's height and
/// curvature and where it fades out are its mesh's, and the colour is the fog lifted by what its own
/// daylight says a cloud is worth against the air.
///
/// **Alpha is coverage and the colour is emission**, which is the material the engine gives it: an
/// unlit alpha-tracking one, so a cloud owes nothing to where the sun is and a thin cloud lets the
/// sky through rather than lightening it.
///
/// @param covered how much of what lies behind the deck it hides, which is what puts the stars out.
vec3 cloudDeck(vec3 direction, out float covered)
{
    covered = 0.0;
    if (!(frame.mClouds.mOpacity > 0.0) || frame.mClouds.mTexture == NO_TEXTURE || direction.z <= 0.0)
        return vec3(0.0);

    // Turned about the zenith first, so the deck runs the way the weather drives it.
    const float turn = frame.mClouds.mTurn;
    const vec2 bearing = vec2(cos(turn), sin(turn));
    const vec2 plane = direction.xy / direction.z;
    const vec2 along = vec2(plane.x * bearing.x - plane.y * bearing.y, plane.x * bearing.y + plane.y * bearing.x);

    // **How far down the layer has fallen where this ray meets it.** A flat one is met at `xy / z`
    // and a curved one nearer, by the height of its crossing: solving `r = t (h - k r²)` gives
    // `h · 2 / (1 + sqrt(1 + 4kh t²))`, which is the conjugate of `(sqrt(1 + 4kh t²) - 1) / 2kt` and
    // is written that way because the other form divides by nothing on a ray straight up — and by a
    // curvature of nothing on the flat layer a replaced mesh may be.
    const float fallen = 2.0 / (1.0 + sqrt(1.0 + 4.0 * frame.mClouds.mCurvature * dot(plane, plane)));

    const vec2 laid = along * frame.mClouds.mTiles * fallen;

    // **How far out the ray met the layer, which is what the engine's fade is written in.** It paints
    // the outermost ring of its mesh at nothing and the one inside it at a quarter, and a triangle
    // between two rings interpolates that linearly in position — so the fade is a straight line in
    // this radius, and the three radii it turns on came off the same mesh.
    const vec3 rings = frame.mClouds.mRings;
    const float reach = length(laid);
    if (reach >= rings.z)
        return vec3(0.0);

    // Two clamped ramps nested rather than a pair of branches: the inner one carries the deck from
    // whole to a quarter and the outer takes what is left of it to nothing, and each is already at
    // its own end wherever the other is running. A band the rule left empty divides by nothing.
    const float inner = clamp((reach - rings.x) / max(rings.y - rings.x, 1.0e-6), 0.0, 1.0);
    const float outer = clamp((reach - rings.y) / max(rings.z - rings.y, 1.0e-6), 0.0, 1.0);
    const float reaches = mix(mix(1.0, CLOUD_RING_ALPHA, inner), 0.0, outer);

    const vec2 uv = laid + vec2(0.0, frame.mClouds.mScroll);

    // **The top mip and no cone.** A deck seen edge-on wants a level off the ray's gradient, and the
    // gradient is what the hardware works out for itself from neighbouring lanes, which a ray tracer
    // does not have. The engine's own fade is what stands in: the band where the stretch would alias
    // is the band it takes the deck out over.
    const vec4 near = textureLod(textures[nonuniformEXT(frame.mClouds.mTexture)], uv, 0.0);
    const vec4 far = frame.mClouds.mNext == NO_TEXTURE
        ? near
        : textureLod(textures[nonuniformEXT(frame.mClouds.mNext)], uv, 0.0);

    const vec4 cloud = mix(near, far, frame.mClouds.mBlend);

    covered = cloud.a * reaches * frame.mClouds.mOpacity;

    // **And the colour crosses to the air over the same stretch**, which is the second thing the
    // engine does with that vertex alpha: `paintClouds` mixes the deck toward the fog by it, so the
    // last rings of cloud are the horizon's own colour rather than a thin wash of a distant one.
    return mix(frame.mSkyHorizon, frame.mClouds.mColour, reaches) * covered;
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
        // they share: a direction at the limb stands `sin(radius)` off the centre line.
        const float limb = sin(sheet.mAngularRadius);
        const vec2 at = vec2(dot(direction, sheet.mRight), dot(direction, sheet.mUp)) / max(limb, 1.0e-4);
        if (dot(at, at) >= 1.0)
            continue;

        const vec4 texel = textureLod(textures[nonuniformEXT(sheet.mTexture)], 0.5 + 0.5 * at, 0.0);
        painted += texel.rgb * (texel.a * NEBULA_RADIANCE);
    }

    return painted;
}

/// What the star field sends back along a ray.
///
/// **The mapping is the engine's own mesh's, measured at load rather than chosen.** The night mesh
/// lays its sheet over a dome at a fixed amount of texture per radian of sky — the same amount
/// across and up, which is what keeps a star round — so this is that unwrap without the mesh:
/// `mTile` of sky to one tile, in azimuth and in the angle down from the zenith alike. Morrowind's
/// comes to a texel a tenth of a degree wide, which is a star; the same sheet spread once over the
/// hemisphere is a third of a degree, which is a blob.
///
/// **And the fade at the horizon is the mesh's too**, not a number picked to look right: the engine
/// draws a vertex of that dome only where its authored colour is white, and the bottom ring alone is
/// not — so the field goes out between the horizon and the ring above it.
///
/// What this leaves out is the other six meshes in that file: three nebulae and the warrior, mage
/// and thief constellations, each over its own band of sky.
vec3 starField(vec3 direction)
{
    if (!(frame.mStars.mFade > 0.0) || frame.mStars.mTexture == NO_TEXTURE || direction.z <= 0.0)
        return vec3(0.0);

    const float elevation = asin(clamp(direction.z, -1.0, 1.0));
    const float reaches
        = frame.mStars.mHorizon > 0.0 ? clamp(elevation / frame.mStars.mHorizon, 0.0, 1.0) : 1.0;
    if (reaches <= 0.0)
        return vec3(0.0);

    // The roll is a turn of the sphere, which in this unwrap is a shift along `u` and nothing else.
    const float azimuth = atan(direction.y, direction.x) - frame.mStars.mTurn;
    const vec2 uv = vec2(azimuth, 0.25 * TAU - elevation) / frame.mStars.mTile;

    // **Premultiplied by its own alpha, which is how the engine lays this sheet on.**
    // `paintAtmosphereNight` hands the texture's alpha to a `(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)` blend,
    // and the sheet is 99% transparent — so reading the colour and dropping the alpha draws the
    // black between the stars as though it were sky.
    const vec4 sheet = textureLod(textures[nonuniformEXT(frame.mStars.mTexture)], uv, 0.0);

    return (frame.mStars.mFade * reaches * STAR_RADIANCE * sheet.a) * sheet.rgb;
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

    // Where across the face, in units of its radius. A direction at the limb stands `sin(radius)`
    // off the centre line, so dividing by that puts the limb at one and makes the cone test a
    // comparison this needed anyway.
    const float limb = sin(moon.mAngularRadius);
    const vec2 at = vec2(dot(direction, moon.mRight), dot(direction, moon.mUp)) / limb;
    const float across = length(at);

    // The pixel's own spread in the same units, so the silhouette is antialiased rather than
    // stepped. A moon is degrees wide and a pixel a thousandth of one, so this is a hair either
    // side of the limb and nothing anywhere else.
    const float fade = max(blur / limb, 1.0e-5);
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
/// @param blur how far this ray's cone has spread from its axis, in radians.
vec3 skyRadiance(vec3 direction, float blur)
{
    // **Everything on or beyond the celestial sphere first, which is what a moon stands in front
    // of.** The stars and the patches are on that sphere and the sun is nearer, but both are further
    // off than a moon and neither is ever between two of them, so one term carries the pair.
    vec3 colour = frame.mStars.mFade > 0.0 ? starField(direction) + frame.mStars.mFade * skyPatches(direction)
                                           : vec3(0.0);

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
    if (frame.mSunIrradiance != vec3(0.0) && length(direction - frame.mSunPosition) < edge)
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
    for (uint moon = 0u; moon < 2u; ++moon)
    {
        float covered;
        const vec3 face = moonFace(frame.mMoons[moon], direction, blur, covered);
        colour = colour * (1.0 - covered) + face;
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
    const vec3 clouds = cloudDeck(direction, covered);

    return colour * (1.0 - covered) + clouds;
}

#endif
