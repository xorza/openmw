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
/// **This is the sky as a source of light**, which is not the sky as a thing to look at. What a
/// bounce gathers must leave out the sun's disc: `gather` asks the sun directly, so a hemisphere
/// that also picked it up out of the sky would count it twice — and a bounce cone a radian wide
/// would pick it up over a quarter of the sky at that.
vec3 skyGlow(vec3 direction)
{
    return skyGradient(frame.mSkyHorizon, frame.mSkyZenith, direction);
}

/// How wide one tile of the cloud texture is across the sky.
///
/// The deck is a flat layer, so a direction meets it at `xy / z` — the tangent of its angle from
/// straight up — and one at the zenith puts a tile's edge at forty-five degrees. Morrowind's own
/// dome is about that: a handful of tiles from horizon to horizon.
const float CLOUD_TILE = 1.0;

/// Where the deck stops being drawn and starts being the haze it fades into, as a height above the
/// horizon.
///
/// **A flat layer's horizon is at infinity**, which is where `xy / z` goes as the ray levels out —
/// so the tiles compress without bound and the texture turns to noise before they do. That is also
/// what Morrowind's sky actually looks like, because the same place is where its fog takes over: the
/// horizon *is* the fog, `mSkyHorizon` is that colour, and the deck arrives at it rather than being
/// cut off in front of it.
const float CLOUD_HORIZON = 0.28;

/// What a ray that reached nothing finds in the cloud deck, and how much of the sky it hides.
///
/// **The deck is where the ray crosses a layer at a height**, which is the thing the game's cloud
/// mesh was a stand-in for. What is Morrowind's here is everything except that surface: the texture
/// is the weather's own, the scroll and the bearing are what the engine turns its mesh by, the blend
/// across a transition is the same factor, and the colour is the fog plus the eighth
/// `SkyManager::setWeather` adds to it.
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

    // Into the haze rather than off an edge, for the reason `CLOUD_HORIZON` gives.
    const float reaches = smoothstep(0.0, CLOUD_HORIZON, direction.z);
    if (reaches <= 0.0)
        return vec3(0.0);

    // Turned about the zenith first, so the deck runs the way the weather drives it.
    const float turn = frame.mClouds.mTurn;
    const vec2 bearing = vec2(cos(turn), sin(turn));
    const vec2 along
        = vec2(direction.x * bearing.x - direction.y * bearing.y, direction.x * bearing.y + direction.y * bearing.x);

    const vec2 uv = along / (direction.z * CLOUD_TILE) + vec2(0.0, frame.mClouds.mScroll);

    // **The top mip and no cone.** A deck seen edge-on compresses without bound, so the right level
    // is whatever the gradient says — and the gradient is what the hardware works out for itself
    // from neighbouring lanes, which a ray tracer does not have. The horizon fade is what stands in:
    // it takes the texture out over exactly the band where the compression would alias.
    const vec4 near = textureLod(textures[nonuniformEXT(frame.mClouds.mTexture)], uv, 0.0);
    const vec4 far = frame.mClouds.mNext == NO_TEXTURE
        ? near
        : textureLod(textures[nonuniformEXT(frame.mClouds.mNext)], uv, 0.0);

    const vec4 cloud = mix(near, far, frame.mClouds.mBlend);

    covered = cloud.a * reaches * frame.mClouds.mOpacity;
    return frame.mClouds.mColour * covered;
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
        painted += texel.rgb * texel.a;
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

    return (frame.mStars.mFade * reaches) * textureLod(textures[nonuniformEXT(frame.mStars.mTexture)], uv, 0.0).rgb;
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
/// @param covered how much of what lies behind the moon it hides, which is what puts the sun out.
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

    return base * MOON_RADIANCE * shade * covered;
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
    vec3 colour = skyGlow(direction);

    // **The stars are behind everything and the deck is in front of it**, which is the order the
    // sky is actually stacked in: a star is on the celestial sphere, the clouds are a couple of
    // kilometres up, and the moons and the sun are between them. So the stars go on first, the deck
    // takes its share of whatever is behind it at the end, and the two discs are added in between.
    colour += frame.mStars.mFade > 0.0 ? starField(direction) + frame.mStars.mFade * skyPatches(direction)
                                         : vec3(0.0);

    // **Added to the sky rather than composited over it**, because what stands between the eye and
    // a moon is air, and the dome's own glow is that air. What the moon *does* hide is anything
    // further off than it is, which for now is the sun alone.
    float hidden = 0.0;
    for (uint moon = 0u; moon < 2u; ++moon)
    {
        float covered;
        colour += moonFace(frame.mMoons[moon], direction, blur, covered);
        hidden = max(hidden, covered);
    }

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
        const float radiance = brightest(frame.mSunIrradiance) / (0.5 * TAU * edge * edge);

        // **Dimmed by whatever stands in front of it, which is the whole of an eclipse.** Masser is
        // nineteen degrees across against the sun's half a degree, so on the rare crossing it is
        // total, and it costs one multiply on the frames it is not.
        colour += ((1.0 - hidden) * radiance) * frame.mSunDiscColour;
    }

    // Last, and over everything: the deck is nearer than any of it.
    float covered;
    const vec3 clouds = cloudDeck(direction, covered);

    return colour * (1.0 - covered) + clouds;
}

#endif
