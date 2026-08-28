// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_SEA_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_SEA_GLSL

// The sea's tiles and everything read out of them: the surface's normal, what a cone could not
// resolve of its slope, and the caustics its curvature focuses.
//
// **One height field, differentiated twice.** The normal is its gradient and the caustics are its
// curvature; the moment either sampled a field of its own the light would land where the surface is
// not. `WavePass` transforms all three out of one spectrum for that reason, and this reads them.
// Nothing in here is lit — that is `water.glsl`.

#include "scene.h"
#include "wave.h"
#include "bindings.glsl"
#include "random.glsl"


/// How far refraction deflects a ray, per unit of surface slope.
///
/// A tilted surface bends light toward its normal, and the deflection is the difference between the
/// angles of incidence and refraction — which for small angles is the slope times this. Derived from
/// the index of refraction rather than written out, so nothing here can come to disagree about what
/// water is.
const float WATER_REFRACTION_BEND = 1.0 - 1.0 / WATER_IOR;

/// The scale of the pattern at the focus, in world units, which it grows from.
///
/// **Snyder and Dera's other half, and the one a blur cannot supply.** Their measurement is that the
/// dominant frequency of the fluctuation falls as the inverse square root of the depth — so the
/// pattern's own scale grows as the root of it, which is branching and not any kind of blurring.
/// Both blur terms are linear in the depth and still under one texel at three metres, so left to
/// them nothing at all changed across the shallows anyone looks at.
///
/// **Fitted against the law rather than derived.** The contrast from two metres to six comes out at
/// 0.60 of itself here against the 0.58 the root asks for, and a larger grain overshoots it — 0.36
/// at twelve. It lands within a texel of the wider tile, which is the finest the transform carries
/// and so the finest a pattern read off it could have had.
const float WATER_CAUSTIC_GRAIN = 8.0;

/// How wide a patch of surface a point one unit down gathers its light from, per unit of depth.
///
/// **Why a caustic coarsens as the water deepens.** Two things blur it and both are angles, so both
/// grow with the depth: the sun is a disc rather than a point, and the surface presents a spread of
/// slopes. Together they say a point at depth `d` is lit by a patch this many units across, and
/// reading the tiles at that footprint is what broadens the pattern as the water deepens. Both are
/// geometry and both are linear in the depth, which is why they are not the whole of the coarsening:
/// `WATER_CAUSTIC_GRAIN` carries the part that is not a blur.
///
/// The sun's term is its angular *diameter*, narrowed by refraction on the way in. A mip chain
/// preserves the mean, so nothing here changes how much light arrives.
const float WATER_CAUSTIC_SPREAD = 2.0 * SUN_ANGULAR_RADIUS / WATER_IOR;

/// The depth a sea's caustics are boldest at, in world units.
///
/// **Measured rather than derived, because the sea this renderer synthesises cannot find its own.**
/// A real ocean's curvature is dominated by waves far shorter than `sShortestWave` — ripples and
/// capillaries — so its first focus lies within a metre of the surface, which is where Snyder and
/// Dera found the maximum of the light fluctuation in 1970 and where every field measurement since
/// has put it. The transform stops at half a metre of wavelength, so left to itself it focuses at
/// eight, and a bed at six metres came out bolder than one at two. A metre and a half here, which
/// is the shallow end of what the measurements report.
///
/// **And the carried pattern is normalised to reach its own fold here**, which is the other half of
/// saying the sea is band-limited. The tiles hold about a fifth of a real sea's curvature, so run at
/// the literal deflection they would draw a pattern a fifth as bold as the water has — faint at
/// every depth rather than only at the wrong ones. Scaling instead so the fold lands at the focus
/// gives the light the strength it is measured to be redistributed with, drawn with the shape the
/// transform can carry.
const float WATER_CAUSTIC_FOCUS = 100.0;

/// How much of the pattern is drawn, as a share of its own departure from a flat sea.
///
/// **The one number here that answers taste rather than a measurement, and it says so.** Everything
/// else in this file is the sea differentiated or a figure taken off it; this is how much of the
/// lens to show. What the arithmetic gives is the whole of it, and the whole of it reads brighter on
/// a Morrowind shore than the game wants.
///
/// **It scales the departure from one and never the light.** `causticGain` makes the pattern average
/// to exactly one, and a share of a thing that averages to one still averages to one — so this can be
/// turned anywhere between nothing and the full lens without the bed receiving a photon more or less
/// than falls on the water. Multiplying the caustic instead would have taken the light with it.
///
/// The ceiling is not this dial and cannot be. Cutting the cusps lower makes `causticGain` divide by
/// less, which puts the peak straight back: at a ceiling of 1.4 the brightest place on the bed comes
/// out where it was, with a gentler shape under it.
const float WATER_CAUSTIC_STRENGTH = 0.4;

/// How fast the pattern fades past the focus, as a power of the depth.
///
/// **A half is what the sea was measured at.** Snyder and Dera's law is that the amplitude of the
/// fluctuation and its dominant frequency both fall as the inverse square root of the depth, and
/// that is what a measurement of the ocean says. One is twice that exponent, so the pattern is gone
/// by twenty metres where the water still has light in it — chosen for the look and not found in
/// the sea, which is worth saying out loud beside a file full of numbers that were.
///
/// **Blending toward one rather than scaling is what keeps the light wherever this is set**, so the
/// exponent is free to be turned and the mean does not follow it. Measured at two, six and twenty
/// metres: 0.213, 0.064 and 0.014 of contrast, where a half leaves the deep end four times bolder.
const float WATER_CAUSTIC_FADE = 1.0;

/// How far toward its own fold the pattern is run at the focus, as a share of the way there.
///
/// **Past one, which is past where a lens has one answer.** At one the determinant first reaches
/// zero; beyond it the map folds over and a point on the bed is reached by three patches of surface
/// where this draws one of them. That is what puts the contrast into thin bright filaments, and this
/// is the dial for how thin they are.
///
/// **Conservation is not what limits it any more.** Run to three, the estimator makes between 13 and
/// 32 per cent of light depending on how coarsely the cone reads the curvature, and `causticGain` is
/// the mean of exactly that divided back out. What the fold still costs is coherence: a filament is
/// the finest thing in the field, so it is made of the fastest-turning waves and it is what moves
/// first — 67 per cent of the pattern is new a twelfth of a second later, of which the sea carries
/// 14 points shoreward rather than replacing them.
const float WATER_CAUSTIC_FOLD = 3.0;

/// How far apart the rain's impacts are, in world units: a lattice with one splash a cell.
///
/// **How many rings is not how many drops.** A real rain lands thousands of drops a second on a
/// square metre and a surface cannot show them as separate rings; what an eye picks out is a few
/// tens. Twenty units is a dozen impacts on a square metre, with a handful of them ringing at any
/// moment.
const float RAIN_RING_CELL = 20.0;

/// How long one ring lasts before it has spread into nothing, in seconds.
const float RAIN_RING_LIFE = 0.6;

/// How fast a ring spreads, in world units a second.
///
/// Capillary-gravity waves on water cannot travel slower than 0.23 m/s — where the surface-tension
/// and the gravity branches of the dispersion relation meet — and a splash ring runs out at about
/// twice that. Thirty-five units is half a metre a second, so a ring reaches thirty centimetres
/// before its life is up.
const float RAIN_RING_SPEED = 35.0;

/// The ring's own wavelength, in world units: eleven centimetres, the scale capillary ripples take.
const float RAIN_RING_LENGTH = 8.0;

/// How steep a fresh ring is, as slope at its crest.
///
/// Per ring, and rings overlap — nine cells are summed — so what it comes to as a field is what is
/// compared against the sea: an rms slope of about a fifth, a third of a running sea's. Enough to
/// break a reflection where a drop lands, and gone again within the ring's life.
const float RAIN_RING_STEEPNESS = 0.30;

/// What the rain adds to the water it lands on, as a slope beside the wave field's.
///
/// **Rings from a lattice of impacts, the same trick the drops themselves use.** Each cell of the
/// water plane holds one impact at a hashed place, repeating on its own phase, and what it leaves
/// is a ring expanding at `RAIN_RING_SPEED` and dying at `RAIN_RING_LIFE`. The slope points away
/// from the impact, because that is what a ring is. The eight neighbours are read as well as the
/// cell itself, because a ring outlives its own cell.
///
/// **Beside the spectrum and not in it.** The caustic differentiates the swell a second time, and a
/// ring eleven centimetres across carries none of that. What the cone could not resolve of a ring
/// is lost slope like any other, though, and joins `WaterSurface::mLostSlope`: rain on far water is
/// a duller sheet, not a mirror.
///
/// @param lost receives the mean square slope the cone averaged away.
vec2 rainSlope(vec2 at, float footprint, out float lost)
{
    lost = 0.0;
    if (!(frame.mRainOnWater > 0.0))
        return vec2(0.0);

    // How much of a ring this cone can still tell apart, from all of it to none of it: a cone a
    // wavelength wide covers a crest and a trough whose slopes cancel, and picking one of them
    // instead is what makes far water a field of crawling sparks.
    const float detail = 1.0 - smoothstep(0.25 * RAIN_RING_LENGTH, 0.75 * RAIN_RING_LENGTH, footprint);
    const float dropped = 1.0 - detail * detail;
    const float wavenumber = TAU / RAIN_RING_LENGTH;

    vec2 slope = vec2(0.0);
    const ivec2 base = ivec2(floor(at / RAIN_RING_CELL));
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            const ivec2 cell = base + ivec2(x, y);

            // Keyed on the cell alone, so an impact stays where it fell from one frame to the next.
            // `randomSeed` mixes the frame in and is exactly what this must not use.
            uint state = pixelKey(uvec2(cell)) * 0x9E3779B9u;

            const vec2 jitter = vec2(randomNext(state), randomNext(state));
            const float offset = randomNext(state);

            const vec2 fell = (vec2(cell) + jitter) * RAIN_RING_CELL;

            // Where this impact is in its own life, which each cell keeps its own phase of so the
            // whole surface does not ring at once.
            const float age = fract(frame.mTime / RAIN_RING_LIFE + offset) * RAIN_RING_LIFE;

            const vec2 away = at - fell;
            const float distance = length(away);
            const float front = RAIN_RING_SPEED * age;
            if (distance > front || !(front > 0.0))
                continue;

            // Fading as it spreads — its energy over a circumference that grows — and as its life
            // runs out.
            const float faded = (1.0 - age / RAIN_RING_LIFE) * front / max(distance, 0.15 * front);
            const float steepness = RAIN_RING_STEEPNESS * frame.mRainOnWater * min(faded, 1.0);

            // A sinusoid of steepness `s` has mean square slope `s^2 / 2`, which is the argument the
            // sea's own chain makes for the swell and holds here unchanged.
            lost += dropped * 0.5 * steepness * steepness;
            if (!(detail > 0.0))
                continue;

            slope += away / max(distance, 1.0e-4) * (detail * steepness * cos(wavenumber * (distance - front)));
        }

    return slope;
}

/// Where on a tile a world position falls. Wrapped by the sampler, because a tile repeats.
vec2 waveCoordinate(uint cascade, vec2 at)

{
    return at / frame.mWaveExtent[cascade];
}

/// What share of the sea's whole curvature a tile still carries at this level of its chain.
///
/// **Read from a table rather than differenced out of the chain.** The fold `causticGain` is a mean
/// against has to be an ensemble quantity: a footprint's own mean square is largest exactly where the
/// curvature under it is largest, so an estimate made that way is smallest on the pixels that need
/// the largest divisor. `Rtx::waveCurvature` states it over the amplitudes instead.
///
/// **The two levels' shares are blended where the sampler blends their values.** That stands in for
/// the variance of the blend, which is not the blend of the variances — but adjacent levels are two
/// boxes a factor of two apart and their transfers stay strongly correlated, so the two agree to a
/// few per cent at the worst blend and exactly at either end of it.
float resolvedShare(uint cascade, float level)
{
    const float last = float(WAVE_LEVELS - 1u);
    const float held = clamp(level, 0.0, last);
    const uint below = uint(held);
    const uint above = min(below + 1u, WAVE_LEVELS - 1u);

    return mix(frame.mWaveResolved[cascade * WAVE_LEVELS + below],
        frame.mWaveResolved[cascade * WAVE_LEVELS + above], held - float(below));
}

/// Which level of a tile's chain a cone this wide can still tell apart.
///
/// **A mip level is the logarithm of a footprint, and that is the whole of the cone's arithmetic.**
/// A level of the chain is the mean of the four texels above it, so the level whose texels are as
/// wide as the cone carries the mean of exactly what the cone covers. Never above the finest level:
/// a cone narrower than a texel has nothing further to be shown.
float waveLevel(uint cascade, float footprint)
{
    const float texel = frame.mWaveExtent[cascade] / float(textureSize(waveSurface[cascade], 0).x);

    return max(log2(footprint / texel), 0.0);
}

/// The water's surface where a ray met it: one read of each tile, and everything taken from it.
///
/// **The normal, the elevation and what the cone could not resolve of either, out of three fetches a
/// tile.** They are derivatives and moments of a single height field, and the moment two of them are
/// computed apart the light lands where the surface is not.
struct WaterSurface
{
    /// Unit, from the gradient of the height field.
    vec3 mNormal;

    /// Mean square slope the cone averaged away. **Not gone, rough.** A surface that lost its slope
    /// reflects like polished plastic; keeping the variance of what was dropped is what lets it come
    /// back as a widened specular lobe instead, which is LEAN mapping's argument in one dimension.
    float mLostSlope;
};

/// Reads the surface at a point.
///
/// **Both come out of one fetch**, which is what the moments were composed into that texture for:
/// `E[f^2] - E[f]^2` off one level of one chain is the variance that level averaged away, where a
/// filter applied here would be a second opinion about the same field.
WaterSurface waterSurfaceAt(vec2 at, float footprint)
{
    WaterSurface surface;
    surface.mLostSlope = 0.0;

    vec2 slope = vec2(0.0);

    // **The tiles add rather than one of them being chosen.** Each carries the whole spectrum at
    // half its variance, so the sum is one sea of the roughness asked for — and their widths are not
    // multiples of one another, so the sum repeats only at a distance no frame contains.
    for (uint cascade = 0u; cascade < WAVE_CASCADES; ++cascade)
    {
        const vec2 uv = waveCoordinate(cascade, at);
        const float level = waveLevel(cascade, footprint);

        const vec3 field = textureLod(waveSurface[cascade], uv, level).xyz;

        slope += field.xy;

        // The floor is the chain's own rounding and nothing else: both means are half floats, and
        // the difference of two nearly equal ones can land a hair under nought.
        surface.mLostSlope += max(field.z - dot(field.xy, field.xy), 0.0);
    }

    // What the rain adds on top, which is not part of the spectrum and must not be. Its lost share
    // joins the spectrum's, because a cone that cannot resolve a ring lost real slope either way.
    float rainLost;
    slope += rainSlope(at, footprint, rainLost);
    surface.mLostSlope += rainLost;

    surface.mNormal = normalize(vec3(-slope, 1.0));
    return surface;

}

/// How much the sunlight reaching `depth` below the surface has been gathered, as a multiplier.
///
/// **Caustics are ray density**, and a change in density is the determinant of the Jacobian of the
/// map from where light met the surface to where it landed. For small slopes that map is
/// `q = p - bend * grad(h)`, so its Jacobian is `I - bend * H` with `H` the Hessian of the same
/// height field the normals come from — `bend` rising with the depth, and `WATER_CAUSTIC_FOCUS`
/// setting what it rises to. That is why the transform composes the
/// curvature into a texture of its own rather than leaving it to be differenced here. No photons, no
/// buffer, no noise: the light is where the arithmetic says it is.
///
/// The small-angle approximation is the right one for this game. Vvardenfell's water is thirty
/// metres deep at its very worst and a few at the shore, with slopes under a seventh, so the exact
/// refraction and its linearisation differ by less than the sun's own width.
///
/// **One index of refraction and not three.** Water's runs 1.3326 to 1.3392 across the visible band
/// by Cauchy's fit, so blue turns harder than red and a real caustic has coloured edges; the cost of
/// drawing them would be three determinants over a Hessian that does not depend on the channel.
/// Measured on the reference renderer at this sea state, twelve pixels in ninety thousand came out
/// differing by more than one level. It is what would put prism edges on cusps if the surface ever
/// got steep enough for the determinant to approach zero, and it goes in when it does.
///
/// @param at **where the light met the surface, and not where it landed.** The map above runs from
///        one to the other, so the Jacobian belongs at `p` — and the caller has `p` for nothing,
///        having already worked out the refracted path to charge it for absorption. Read at the
///        landing point instead, the pattern cannot slide as the depth grows and the sun's own
///        direction never enters it at all: a moon drew the sun's caustics, and a bed at two metres
///        and one at thirty drew the same ones.
/// @param depth how far below the surface the light then travelled, in world units.
float caustic(vec2 at, float depth, float footprint)
{
    // Three numbers rather than four, because a Hessian is symmetric: xx, yy, and the shared
    // off-diagonal.
    vec3 hessian = vec3(0.0);
    float resolved = 0.0;

    // **Three limits, and the largest of them wins.** The pixel's own cone; the blur two angles put
    // on the pattern, which grows with the depth because both are angles; and the scale of the
    // pattern itself, which grows as the *square root* of the depth once the map has folded — that
    // is Snyder and Dera's other half, and it is a property of branching rather than of any blur.
    // The root is what makes the change start in the shallows, where a linear term is still under
    // one texel and shows nothing at all.
    const float blurred = depth * (WATER_CAUSTIC_SPREAD + WATER_REFRACTION_BEND * frame.mWaveSlope);
    const float branched = WATER_CAUSTIC_GRAIN * sqrt(depth / WATER_CAUSTIC_FOCUS);
    const float widened = max(footprint, max(blurred, branched));

    for (uint cascade = 0u; cascade < WAVE_CASCADES; ++cascade)
    {
        const vec2 uv = waveCoordinate(cascade, at);
        const float level = waveLevel(cascade, widened);

        hessian += textureLod(waveCurvature[cascade], uv, level).xyz;
        resolved += resolvedShare(cascade, level);
    }

    // One determinant and not a ratio of two, because this surface is not displaced: the quad stays
    // flat and only its normal moves, so the patch of surface the light left is the patch of
    // parameter space it came from. A Gerstner sea gathers toward its own crests before the light
    // ever reaches it, and would need `det(I + dD)` over the numerator to keep a depthless puddle
    // from brightening its own bottom.

    // Rising with the depth to the fold and held there: `bend * rms curvature` reaches one at the
    // focus, which is where a lens is at its strongest and where one Jacobian stops describing what
    // is behind it. The fade below carries the depth from there on.
    const float toward = WATER_CAUSTIC_FOLD * min(depth / WATER_CAUSTIC_FOCUS, 1.0);
    const float bend = toward * inversesqrt(max(frame.mWaveCurvature, 1.0e-12));
    const float determinant
        = (1.0 - bend * hessian.x) * (1.0 - bend * hessian.y) - bend * bend * hessian.z * hessian.z;

    // **A reciprocal of something that fluctuates is worth more than the reciprocal of its mean**,
    // and left alone that is a bed lit brighter than the water over it lets through. `causticGain`
    // is that mean and dividing by it is what leaves the pattern redistributing the sun exactly.
    //
    // **How far the map has folded at the level this pixel reads, which is not `toward`.** `bend` is
    // sized against the sea's whole curvature and the Hessian above is only what the cone could
    // resolve, so the fold the estimator stands at is `toward` scaled by the share still resolved.
    // It goes to nought as the cone reaches the coarse levels, which is where a surface averaged
    // flat has no gain to remove.
    //
    // The gain varies only with how much of the surface the cone can see, so it is flat across a
    // footprint and every bright line and dark cell survives it untouched.
    //
    // The floor on the determinant is what the ceiling means, so there is one number to state.
    const float fold = toward * sqrt(resolved);
    const float gathered = 1.0 / (max(abs(determinant), 1.0 / WATER_CAUSTIC_MAX) * causticGain(fold));

    // **Snyder and Dera's law, and the whole of why deep water has none of this.** Past the focus a
    // point on the bed is reached by several patches of surface at once and this draws one of them,
    // the rest averaging to the mean — so the pattern fades as the inverse square root of the depth
    // while `WATER_CAUSTIC_SPREAD` broadens it by the same power. Measured in the sea since 1970 and
    // found again by every field campaign after it, over depths of one metre to twenty-five.
    //
    // Blending toward one rather than scaling is what keeps the light where it was.
    const float shown = WATER_CAUSTIC_STRENGTH * pow(max(depth / WATER_CAUSTIC_FOCUS, 1.0), -WATER_CAUSTIC_FADE);

    return 1.0 + (gathered - 1.0) * shown;
}

#endif
