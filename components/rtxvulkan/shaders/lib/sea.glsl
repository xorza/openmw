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

#include "look.h"
#include "scene.h"
#include "wave.h"
#include "bindings.glsl"
#include "footprint.glsl"
#include "random.glsl"

/// How far refraction deflects a ray, per unit of surface slope.
///
/// A tilted surface bends light toward its normal, and the deflection is the difference between the
/// angles of incidence and refraction — which for small angles is the slope times this. Derived from
/// the index of refraction rather than written out, so nothing here can come to disagree about what
/// water is.
const float WATER_REFRACTION_BEND = 1.0 - 1.0 / WATER_IOR;

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

    const float detail = resolved(RAIN_RING_LENGTH, footprint);

    // Squared, because `detail` scales a slope and what `lost` accumulates is a mean square of one.
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
            uint state = steppedKey(pixelKey(uvec2(cell)));

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

/// A world position in the sea's own frame, which the wind's heading turns.
///
/// The tiles are spread about their own +X; `frame.mSeaHeading` is where that axis points in the
/// world, so a world position is turned back by it before a tile is read.
vec2 seaLocal(vec2 at)
{
    const vec2 heading = frame.mSeaHeading;

    return vec2(heading.x * at.x + heading.y * at.y, heading.x * at.y - heading.y * at.x);
}

/// A slope read in the sea's own frame, turned into the world's.
vec2 seaWorld(vec2 slope)
{
    const vec2 heading = frame.mSeaHeading;

    return vec2(heading.x * slope.x - heading.y * slope.y, heading.y * slope.x + heading.x * slope.y);
}

/// Where on a tile a world position falls. Wrapped by the sampler, because a tile repeats.
vec2 waveCoordinate(uint cascade, vec2 at)
{
    return seaLocal(at) / frame.mWaveExtent[cascade];
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

    // The tiles were read in the sea's frame and the rings are laid in the world's, so the one is
    // turned before the other joins it. The lost slope is a variance and turns with nothing.
    slope = seaWorld(slope);

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
