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
/// metres: 0.54, 0.17 and 0.040 of contrast against the half's 0.63, 0.33 and 0.14.
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
/// first — 62 per cent of the pattern is new a twelfth of a second later, where the sweep the
/// spectrum's short cutoff was chosen on put tearing at half.
const float WATER_CAUSTIC_FOLD = 3.0;

/// How much of the sea's own height the surf line is spread over, and how steep a face counts as
/// one the wave is running into.
///
/// **The first is what stops the surf line being a drawn edge**, and it is Rayleigh's rather than
/// anybody's taste. McCowan's criterion is met or it is not, so on its own it draws a region with a
/// hard rim; the only thing that ever softened that rim was the elevation a *ray cone* had averaged
/// away, which goes to nought the moment the camera is near.
///
/// What is missing is which *wave* is passing. The criterion asks the sea state for a height and the
/// field for an elevation, and never for the height of the wave the point is under — and individual
/// wave heights in a sea are Rayleigh-distributed about `H_s`, with a standard deviation of 0.37 of
/// it. Over `WATER_BREAKER_RATIO` that is 0.47 of `H_s`, or this many times the rms elevation, and
/// it is the width the breaker line genuinely wanders by.
///
/// **The second is what stops the inside of the band being solid.** White water is thrown on the
/// steep face a wave runs into and not over the whole of a surf zone, so how hard a face leans that
/// way is how much of it is aerated. **Against the sea's own rms slope rather than a number of its
/// own**, so a calmer sea foams in a thinner scatter of patches and a rough one in a broader one,
/// which is what a sea state is for.
///
/// **And it has to reach one sparingly.** Foam at noon is brighter than anything else in a frame, so
/// every pixel above about a third of full cover clips to white — a band that reached one across its
/// middle drew a white ribbon with an edge, whatever the coverage under it was doing. At one rms
/// slope, a sixth of the surface is at full cover and the rest is a gradient, which is what breaks a
/// ribbon into the streaks a shore shows.
const float WATER_FOAM_EDGE = 1.88;
const float WATER_FOAM_FACE = 0.4;

/// How thin the water under a raft of bubbles gets before the raft has grounded, in world units.
///
/// **The last edge the surf line had, and it was the water's own.** Foam is shaded on the water
/// surface, and at the waterline there is no surface left to shade — so however softly the criterion
/// faded, the band still ended at the line where the ray stopped finding water. Against a bed that
/// is nearly a plane that line is nearly straight, and a straight white edge is what a shoreline
/// must never be.
///
/// A raft is bubbles deep, so it grounds where the column is about as thin as the raft is: seventeen
/// centimetres here. That is a sliver of a surf zone tens of units wide, and it is the sliver the
/// eye was reading as a drawn line.
const float WATER_FOAM_GROUND = 12.0;

/// How wide one cell of a raft of bubbles is, in world units, and how softly a cell's rim fades.
///
/// **The one field in here that is not the sea differentiated, and it is a raft rather than water.**
/// Everything else about this surface is the wave field asked a question — the normal is its
/// gradient, the surf line its level set, the caustics its curvature. A bubble raft is none of
/// those: what shapes it is the turbulence of the wave that broke, at centimetres, a hundred times
/// finer than anything the transform carries and not a wave at all. So it is a noise, said out loud.
///
/// **Worley's cells and not a smooth noise**, because that is the shape a raft has: closed patches
/// with thin water between them, and edges that close on themselves. Thresholding one against the
/// coverage is what turns a share of a pixel into patches of a share — the same amount of white,
/// laid out the way foam lays out rather than as a gradient across a band.
///
/// Half a metre a cell, and the raft is carried shoreward at the shallow-water celerity
/// of the water it broke in, so it travels with what made it rather than standing still while the
/// sea runs through it.
const float WATER_FOAM_CELL = 40.0;
const float WATER_FOAM_DISSOLVE = 0.22;

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

    /// The gradient `mNormal` was made of, kept because the foam asks which way the surface tilts
    /// and dividing it back out of a normalised vector is a worse answer than not throwing it away.
    vec2 mSlope;
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
    surface.mSlope = slope;
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

/// How far a point is from the nearest bubble in a raft, from nought at one to about one between.
///
/// Worley's F1 over a jittered lattice, nine cells of it, drifting shoreward with the water.
float foamCells(vec2 at, float celerity)
{
    const vec2 drifted = (at - frame.mWaveTravel * (celerity * frame.mTime)) / WATER_FOAM_CELL;
    const vec2 base = floor(drifted);

    float nearest = 2.0;
    for (int row = -1; row <= 1; ++row)
        for (int column = -1; column <= 1; ++column)
        {
            const vec2 cell = base + vec2(float(column), float(row));
            const ivec2 index = ivec2(cell);
            const vec2 seed = vec2(hashToUnit(ivec3(index, 0)), hashToUnit(ivec3(index, 7)));

            nearest = min(nearest, distance(drifted, cell + seed));
        }

    // **Inverted, so that one is the middle of a bubble and nought is the water between.** F1 over a
    // lattice of one point a cell runs to about seven tenths, which is what puts this on the nought
    // to one the coverage is thresholded against.
    return 1.0 - min(nearest / 0.7, 1.0);
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
float foamBreaking(WaterSurface surface, vec2 at, float depth)
{
    // What the sea state breaks in, and what stands between this point and breaking once the wave
    // over it is counted.
    const float margin = breakingDepth(surface) - (depth + surface.mHeight);

    // `P(the unresolved elevation does not make up the margin)`. The smoothstep stands in for the
    // Gaussian's own integral, which it follows to within a hundredth across the three standard
    // deviations that are not already nought or one.
    //
    // **Two widths under one root, and only one of them is the camera's.** What the cone averaged
    // away is a filtering quantity and goes to nought at close range; what the sea is doing does
    // not, and it is the one that decides how wide a surf line is. Adding their variances is what a
    // sum of two independent things is.
    const float spread = WATER_FOAM_EDGE * surface.mRoughness;
    const float noise = sqrt(surface.mLostHeight + spread * spread);

    // **The face the wave is running into, which is where the white water goes.** The surface is
    // travelling along `mWaveTravel`, so its leading face is the one whose elevation falls that way.
    // Nothing here is a texture: it is the same slope the normal came from, asked a third question,
    // and it is what turns a filled band into the lines of white water a shore actually shows.
    const float leading = -dot(surface.mSlope, frame.mWaveTravel);
    const float front = smoothstep(0.0, WATER_FOAM_FACE * frame.mWaveSlope, leading);

    // The raft thins with the water it floats on, which is what keeps the band from ending at the
    // waterline rather than fading into it.
    const float afloat = smoothstep(0.0, WATER_FOAM_GROUND, depth);

    // **What the three terms agree on is how much of the surface is aerated, and the cells are where
    // it goes.** Thresholding the raft against that share keeps the amount and gives it a shape:
    // patches with closed rims, thinning into scattered flecks where the share falls away, rather
    // than one band shading off at its edges.
    const float share = afloat * front * smoothstep(-1.6, 1.6, margin / noise);
    const float cells = foamCells(at, sqrt(WATER_GRAVITY * breakingDepth(surface)));

    // **The share sets where the water line across the raft sits, which is what keeps the amount.**
    // A cell's middle stands highest, so a small share leaves only the crowns of a few bubbles and a
    // large one floods everything but the channels between them.
    //
    // **The line rises from one and never above it**, which is the whole of what stops this drawing
    // on open sea. A ramp centred on the line instead let the crown of every cell through at half
    // cover wherever the share was nought — a white speck on each cell of the whole water, to the
    // horizon, which is what a two-sided threshold against a bounded field always does at its end.
    return smoothstep(1.0 - share, 1.0 - share + WATER_FOAM_DISSOLVE, cells);
}

/// How much of what broke is still foam by the time it has been carried to a point.
///
/// **A wave has to have reached the point at all, which breaking never asks.** That criterion is a
/// statement about water a wave is crossing; every hollow that dips below sea level satisfies it
/// too, and there are a great many of those behind a shoreline. What tells the two apart is the
/// bed: a shore keeps going down, so the wave broke a run-out away and what it made is still here,
/// while a pan is level, so there is nowhere within a run-out deep enough for one to have broken.
///
/// **A depth against a depth, which is what leaves no pole to guard.** Foam lasts a run-out, so the
/// question is whether water a wave could break in lies within one — and the answer is the still
/// depth out there against the depth this sea breaks in. It used to be a distance over a rate of
/// fall, and a rate that reaches nought has to be guarded: the guard switched the surf line off in
/// one step wherever the bed levelled, which drew the foam as white patches with cliffs cut around
/// them along the shape of the ground.
///
/// **The still depth and not the instantaneous one**, which `foamBreaking` is the other way round
/// about. Where the surf line falls is a question about this wave and wanders with it; how wide the
/// surf zone is, is a question about the beach and does not.
///
/// The width is the one `WATER_FOAM_EDGE` gives, for its reason: which wave is out there is a draw
/// from the same Rayleigh distribution, so the answer is as uncertain here as it is at the breaker
/// line itself.
///
/// @param outThere how deep the still water is one run-out down the slope, from
///        `bedDepthDownhill`.
float foamReaching(WaterSurface surface, float outThere)
{
    const float breaking = breakingDepth(surface);
    const float spread = max(WATER_FOAM_EDGE * surface.mRoughness, 1.0e-3);

    return smoothstep(-1.6, 1.6, (outThere - breaking) / spread);
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
    return 1.0 + (gathered - 1.0) * pow(max(depth / WATER_CAUSTIC_FOCUS, 1.0), -WATER_CAUSTIC_FADE);
}

#endif
