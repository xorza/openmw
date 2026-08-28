// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_SEA_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_SEA_GLSL

// The sea's tiles and everything read out of them: the surface's normal, its elevation, what a cone
// could not resolve of either, where it breaks, and the caustics its curvature focuses.
//
// **One height field, differentiated twice.** The normal is its gradient and the caustics are its
// curvature; the moment either sampled a field of its own the light would land where the surface is
// not. `WavePass` transforms all three out of one spectrum for that reason, and this reads them.
// Nothing in here is lit — that is `water.glsl`.

#include "scene.h"
#include "wave.h"
#include "bindings.glsl"

/// How far refraction deflects a ray, per unit of surface slope.
///
/// A tilted surface bends light toward its normal, and the deflection is the difference between the
/// angles of incidence and refraction — which for small angles is the slope times this. Derived from
/// the index of refraction rather than written out, so nothing here can come to disagree about what
/// water is.
const float WATER_REFRACTION_BEND = 1.0 - 1.0 / WATER_IOR;

/// A ceiling on how bright a focus is allowed to get.
///
/// Where the refracted bundle collapses to a line the Jacobian goes to zero and the intensity to
/// infinity — a real caustic *cusp*, and the reason a pool's bright lines are as sharp as they are.
/// Letting one through would put a pixel in the frame that no exposure could hold.
const float WATER_CAUSTIC_MAX = 3.0;

/// How wide a patch of surface a point one unit down gathers its light from, per unit of depth.
///
/// **Why a caustic coarsens as the water deepens.** Two things blur it and both are angles, so both
/// grow with the depth: the sun is a disc rather than a point, and the surface presents a spread of
/// slopes. Together they say a point at depth `d` is lit by a patch this many units across, and
/// reading the tiles at that footprint is what broadens the pattern as the water deepens. Both are
/// geometry: the measured law that the pattern's own scale grows as the *square root* of the depth
/// is what comes out of this and the fade together, not what either states.
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

/// How far toward its own fold the pattern is run at the focus, as a share of the way there.
///
/// **Short of one, because the fold is where the light starts going missing.** At the fold the
/// determinant passes through zero, the ceiling clips the cusp it makes, and what the ceiling took
/// off is light the water received and this did not put anywhere — measured at eight per cent of it
/// in a metre of water. Held here the loss is under two per cent at every depth, which is what
/// `theWavesGatherSunlightOntoTheBedWithoutMakingAnyOfIt` asserts, and the pattern gives up a
/// twentieth of its contrast to do it.
const float WATER_CAUSTIC_FOLD = 0.85;

/// Where on a tile a world position falls. Wrapped by the sampler, because a tile repeats.
vec2 waveCoordinate(uint cascade, vec2 at)
{
    return at / frame.mWaveExtent[cascade];
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

    /// Elevation about the still level, in world units, of the waves this cone can resolve.
    float mHeight;

    /// Mean square slope the cone averaged away. **Not gone, rough.** A surface that lost its slope
    /// reflects like polished plastic; keeping the variance of what was dropped is what lets it come
    /// back as a widened specular lobe instead, which is LEAN mapping's argument in one dimension.
    float mLostSlope;

    /// Variance of the *elevation* the cone averaged away, in world units squared.
    ///
    /// What `mLostSlope` is to the reflection this is to the foam. The surf line is a level set of
    /// the surface, and a level set of a field too fine to resolve is a coin toss unless how far
    /// that field still wanders is carried alongside the mean.
    float mLostHeight;

    /// Root mean square elevation of the whole spectrum, resolved or not — the sea state itself,
    /// and `WATER_SIGNIFICANT_HEIGHT` times it is the height oceanography quotes.
    float mRoughness;
};

/// Reads the surface at a point.
///
/// **Everything in the struct comes out of the same tiles**, which is what the moments were
/// composed into those textures for: `E[f^2] - E[f]^2` off one level of one chain is the variance
/// that level averaged away, where a filter applied here would be a second opinion about the same
/// field. Two fetches at the level the cone reaches, and the tile's own mean beside them.
WaterSurface waterSurfaceAt(vec2 at, float footprint)
{
    WaterSurface surface;
    surface.mHeight = 0.0;
    surface.mLostSlope = 0.0;
    surface.mLostHeight = 0.0;

    vec2 slope = vec2(0.0);
    float variance = 0.0;

    // **The tiles add rather than one of them being chosen.** Each carries the whole spectrum at
    // half its variance, so the sum is one sea of the roughness asked for — and their widths are not
    // multiples of one another, so the sum repeats only at a distance no frame contains.
    for (uint cascade = 0u; cascade < WAVE_CASCADES; ++cascade)
    {
        const vec2 uv = waveCoordinate(cascade, at);
        const float level = waveLevel(cascade, footprint);

        const vec4 field = textureLod(waveSurface[cascade], uv, level);
        const vec4 curve = textureLod(waveCurvature[cascade], uv, level);

        surface.mHeight += field.x;
        slope += field.yz;

        // The floor is the chain's own rounding and nothing else: both means are half floats, and
        // the difference of two nearly equal ones can land a hair under nought.
        surface.mLostHeight += max(field.w - field.x * field.x, 0.0);
        surface.mLostSlope += max(curve.w - dot(field.yz, field.yz), 0.0);

        // The last level is one texel, so it is the tile's own mean square elevation — the sea state
        // itself, taken off the textures the shape comes from rather than handed down beside them.
        variance += textureLod(waveSurface[cascade], uv, WAVE_COARSEST).w;
    }

    surface.mNormal = normalize(vec3(-slope, 1.0));
    surface.mRoughness = sqrt(variance);
    return surface;
}

/// The depth this sea state breaks in, in world units.
///
/// A wave breaks when its height reaches about three quarters of the depth it stands in —
/// McCowan's solitary-wave limit, 1894, still what a surf zone is placed with. It is what makes the
/// band's width a consequence of the sea rather than a distance anyone picks: a calmer sea breaks
/// closer in, in a narrower strip, with nothing tuned.
float breakingDepth(WaterSurface surface)
{
    return surface.mRoughness * WATER_SIGNIFICANT_HEIGHT / WATER_BREAKER_RATIO;
}

/// How far broken water is carried before it has gone, in world units.
///
/// The shallow-water celerity at the breaker line, over the time a raft of bubbles lasts. The floor
/// is a divide guard for a sea too flat to have a surf zone at all, where nothing breaks anyway.
float foamRunout(WaterSurface surface)
{
    return max(WATER_FOAM_LIFETIME * sqrt(WATER_GRAVITY * breakingDepth(surface)), 1.0e-3);
}

/// What share of the surface at a point is breaking, from none of it to all.
///
/// **Against the depth at this instant rather than the still one**, which is the whole of the
/// pattern. A crest carries the column deeper and a trough leaves it thinner, so the criterion is
/// met and unmet as the waves run through — the surf line wanders up and down the beach by the
/// height of the sea rather than sitting where the still water would put it, and it is ragged along
/// the shore because the wave passing over it is. Nothing here is a texture or a noise field: it is
/// the same height the normal came from, asked a different question.
///
/// **And what the cone could not resolve is answered rather than dropped.** Far enough off, the
/// surf line is finer than the pixel looking at it, and a hard test against an averaged height
/// flickers between all foam and none as the camera moves. What is wanted there is the *share* of
/// the surface breaking, which is the Gaussian tail of the elevation that was averaged away — a sum
/// of many independent sinusoids is Gaussian by the central limit theorem. Both ends fall out of one
/// expression once the unresolved elevation is treated as the noise it is, which is the same
/// argument `mLostSlope` makes for the reflection and why the two are carried together.
///
/// @param depth how deep the still water is under this point, straight down and in world units.
float foamBreaking(WaterSurface surface, float depth)
{
    // What the sea state breaks in, and what stands between this point and breaking once the wave
    // over it is counted.
    const float margin = breakingDepth(surface) - (depth + surface.mHeight);

    // `P(the unresolved elevation does not make up the margin)`. The smoothstep stands in for the
    // Gaussian's own integral, which it follows to within a hundredth across the three standard
    // deviations that are not already nought or one. The floor is a divide guard and nothing else:
    // a surface every one of whose waves is resolved has a genuinely sharp edge, and drawing it
    // sharp is right.
    const float noise = max(sqrt(surface.mLostHeight), 1.0e-3);

    return smoothstep(-1.6, 1.6, margin / noise);
}

/// How much of what broke is still foam by the time it has been carried to a point.
///
/// **A wave has to have reached the point at all, which breaking never asks.** That criterion is a
/// statement about water a wave is crossing; every hollow that dips below sea level satisfies it
/// too, and there are a great many of those behind a shoreline. What tells the two apart is the
/// bed: a shore keeps going down, so the wave broke a few metres away and what it made is still
/// here, while a pan is level, so the nearest water deep enough to break in is tens of metres off
/// and nothing that broke there survives the trip. Measured at Seyda Neen the two populations sit a
/// hundred-fold apart with nothing in between, which is why the shape of the fall-off decides
/// nothing that was ever close.
///
/// @param depth how deep the still water is under this point, in world units.
/// @param fall how fast the bed falls away toward deep water, as a tangent — measured over the
///        run-out rather than read off the surface here, for the reason `bedFall` gives.
/// @param runout what `foamRunout` said, handed in because the caller measured `fall` across it and
///        the two have to be the same number.
float foamReaching(WaterSurface surface, float depth, float fall, float runout)
{
    const float breaking = breakingDepth(surface);

    // How far the wave came through water shallow enough to break in: the depth still to be lost,
    // over the rate it is lost at.
    //
    // **The still depth and not the instantaneous one**, which `foamBreaking` is the other way
    // round about. Where the surf line falls is a question about this wave and wanders with it; how
    // wide the surf zone is, is a question about the beach and does not.
    const float crossed = max(breaking - depth, 0.0) / max(fall, 1.0e-4);

    return exp(-crossed / runout);
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
    float traceVariance = 0.0;
    float carried = 0.0;

    // Never finer than the pixel asked for and never finer than the water allows, which are two
    // different limits: the first is what the camera can see and the second is what is there.
    const float widened = max(footprint, depth * (WATER_CAUSTIC_SPREAD + WATER_REFRACTION_BEND * frame.mWaveSlope));

    for (uint cascade = 0u; cascade < WAVE_CASCADES; ++cascade)
    {
        const vec2 uv = waveCoordinate(cascade, at);
        const float level = waveLevel(cascade, widened);

        const vec3 curve = textureLod(waveCurvature[cascade], uv, level).xyz;
        hessian += curve;

        // **How far `tr(H)` still fluctuates at the level this pixel reads**, for the gain below.
        // The coarsest level is the whole tile's mean square and the level here is the footprint's,
        // so what the cone averaged away is the second less the square of the trace it left — and
        // the fluctuation that survives is the whole less that. At the finest level nothing was
        // averaged away and this is the tile's own variance; at the coarsest the surface is flat and
        // there is nothing left to correct.
        const float whole = textureLod(waveVariance[cascade], uv, WAVE_COARSEST).x;
        const float local = textureLod(waveVariance[cascade], uv, level).x;
        const float trace = curve.x + curve.y;

        traceVariance += max(whole - max(local - trace * trace, 0.0), 0.0);
        carried += whole;
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
    const float bend = toward * inversesqrt(max(carried, 1.0e-12));
    const float determinant
        = (1.0 - bend * hessian.x) * (1.0 - bend * hessian.y) - bend * bend * hessian.z * hessian.z;

    // **A reciprocal of something that fluctuates is worth more than the reciprocal of its mean**,
    // and left alone that is a bed lit brighter than the water over it lets through. One patch of
    // surface is read for each patch of *bed*, so the samples are not weighted by the area each one
    // stands for on the surface, and the excess is Jensen's: writing
    // `det = 1 - u + v` with `u = bend * tr(H)` and `v = bend^2 * det(H)`,
    //
    //   E[1 / det] = 1 - E[v] + Var[u] + ...
    //
    // and `E[det H]` is zero for a Gaussian field — the `Hxx Hyy` and `Hxy^2` terms have the same
    // expectation — which leaves `Var[u] = bend^2 * Var[tr H]` as the whole of the second order.
    // That is `traceVariance`, read off a chain that was already being sampled, and it varies only
    // with how much of the surface the cone can see: the gain it removes is flat across a footprint,
    // so every bright line and dark cell survives it untouched.
    //
    // The floor on the denominator is what the ceiling means, so there is one number to state.
    const float gathered = 1.0 / (max(abs(determinant), 1.0 / WATER_CAUSTIC_MAX) * (1.0 + bend * bend * traceVariance));

    // **Snyder and Dera's law, and the whole of why deep water has none of this.** Past the focus a
    // point on the bed is reached by several patches of surface at once and this draws one of them,
    // the rest averaging to the mean — so the pattern fades as the inverse square root of the depth
    // while `WATER_CAUSTIC_SPREAD` broadens it by the same power. Measured in the sea since 1970 and
    // found again by every field campaign after it, over depths of one metre to twenty-five.
    //
    // Blending toward one rather than scaling is what keeps the light where it was.
    return 1.0 + (gathered - 1.0) * inversesqrt(max(depth / WATER_CAUSTIC_FOCUS, 1.0));
}

#endif
