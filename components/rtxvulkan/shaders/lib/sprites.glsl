// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_SPRITES_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_SPRITES_GLSL

// The particle layer, marched against the primary ray rather than built into an
// acceleration structure — and which sprite over a pixel owns its motion.

#include "colour.h"
#include "scene.h"
#include "bindings.glsl"
#include "fog.glsl"
#include "frame.glsl"
#include "lights.glsl"
#include "shading.glsl"

/// What a puff of smoke is lit by, per unit of albedo.
///
/// **As bright as a card of the same albedo held beside it, and that is not a fudge.** An opaque
/// diffuse *sphere* would catch `pi r^2` of the beam and radiate over `4 pi r^2` — a quarter of the
/// facing value — but a puff is neither opaque nor diffuse: it is a cloud of droplets that scatters
/// strongly forward and again inside itself, so the sun reaches all of it rather than one
/// hemisphere. The quarter, tried first in the reference implementation, put a plume back at the
/// sky's own ambient, where it was invisible. That is the *mean*: which side of the puff the sun's
/// share leaves by is `SMOKE_ANISOTROPY`'s, and it arrives here folded into `sunLit`.
///
/// The lamps arrive the way they arrive at the fog — as irradiance spread over the whole sphere —
/// because a puff is the same kind of thing the fog is, only denser and in one place. So it is the
/// same `lampsAt` sum and the same one multiply on it.
///
/// **A particle has no normal and it still has an up**, which is how a sprite is occluded by the
/// world: what a point sees of the sky is a question about the point, and the sky is above. What
/// it has instead of a normal is a side — `ballWrap` — and a thickness its own texture records —
/// `sixWayThrough` — and those arrive folded into the factors below. `spritesAlong` says why the
/// world's part of them is asked once for a layer rather than once for a puff.
///
/// @param daylight what `daylightReaching` says of the position, asked once by the caller.
/// @param lamps what every lamp delivers here unshadowed, out of `lampsAt` — **asked once for the
///        emitter and not once for a puff**. `spritesAlong` says what that costs and why.
/// @param sunLit what the world and the puff itself leave of the sun at this point.
/// @param skyLit the same for the sky over it.
/// @param fillLit what stands between the puff and a room's own fill, which comes from everywhere:
///        the puff's own thickness in every direction, and what the room has standing around it.
/// @param lampLit the same for the lamps — **one shadow answer for every lamp and for the whole
///        layer**, where the sum beside it is the emitter's. Whether a lamp is *seen* changes
///        slowly, and a torch behind a wall is behind it for the whole layer.
vec3 puffLight(vec3 daylight, vec3 lamps, float sunLit, float skyLit, float fillLit, float lampLit)
{
    // `pathEnd`, with the sky's share and the room's share of the ambient each shadowed by its own
    // answer rather than the room's by none: a fill comes from everywhere, so what a puff lets
    // through of it is the mean of every way in.
    return frame.mAmbient * (daylight * mix(fillLit, skyLit, frame.mAmbientFromSky))
        + (HAS_SUN ? frame.mSunIrradiance * (INV_PI * daylight * sunLit) : vec3(0.0))
        + INV_FOUR_PI * lamps * lampLit;
}

/// What a painted alpha hides over `crossings` of the thickness it was painted for.
///
/// **A painted alpha is an optical depth and not a coverage.** A ray through part of one crossing
/// hides less and a ray through more than one hides more, and both are `1 - (1 - a) ^ n`. A sprite's
/// `n` is the share of its own chord the eye sees — one in the open, a sliver where the ball runs
/// into a wall. A shell of medium's is the secant of the angle the ray crosses it at — one head on,
/// and more at a slant. One law, and the three places that used to spell it out are the two kinds of
/// puff and the flame between them.
///
/// `SPRITE_ALPHA_LIMIT` says why an alpha of one is not taken at its word.
float paintedOver(float painted, float crossings)
{
    return 1.0 - pow(1.0 - min(painted, SPRITE_ALPHA_LIMIT), crossings);
}

/// The same per channel, for a flame that absorbs as much as it emits in each of them.
vec3 paintedOver(vec3 painted, float crossings)
{
    return 1.0 - pow(1.0 - min(painted, vec3(SPRITE_ALPHA_LIMIT)), vec3(crossings));
}

/// How a ball is lit from `toward` against its mean, on the side of it the eye sees.
///
/// A zero `toward` — a lamp that is not there — is lit at the mean, because nothing is being asked.
float ballWrap(vec3 normal, vec3 toward)
{
    return 1.0 + SPRITE_WRAP * dot(normal, toward);
}

/// What the world leaves of each of the three lights a layer of puffs has.
struct PuffAbove
{
    /// What stands between the layer and the sun. One where there is no sun to ask about.
    float mSunLit;

    /// The same for whichever ambient the frame has: the sky over the layer, or the room around it.
    float mAmbientLit;

    /// The same for the one lamp the reservoir held, and which way that lamp lies from the layer.
    float mLampLit;
    vec3 mLampToward;
};

/// The three rays a layer of puffs casts, taken once for the layer and not once for a puff.
///
/// **One answer for however many puffs a pixel holds**, which is what makes a layer affordable at
/// all: a shadow ray inside either walk's loop would multiply with the drops a rainstorm puts over a
/// pixel or the shells a cloud does. What stands between a layer and a light changes slowly — a
/// torch behind a wall is behind it for the whole layer — so both walks ask here, at the first puff
/// of theirs that is lit.
///
/// **One place, because these are three decisions and not three calls.** Which ambient a frame has,
/// how far to look for what stands in it, and which lamp of a cell is worth the one ray are answers
/// a sprite and a cloud shell must not reach differently — and they did not stay the same by being
/// written twice.
///
/// **Cosine about the up out of doors, and the whole sphere in a room**, which is where each ambient
/// actually comes from: the sky is above a puff and nowhere else, and a room's fill arrives from
/// every side of it. One ray answers whichever the frame has, because `puffLight` mixes to exactly
/// one of them. Asked in a room too — a puff's own thickness was once the whole of what stood
/// between it and the fill, so smoke under a table came out as bright as smoke in the middle of the
/// floor.
///
/// **The sun comes back unthrown.** `SMOKE_ANISOTROPY` belongs to the angle between the ray and the
/// light rather than to what is in the way, and only the puffs that have a side apply it.
///
/// @param sunSeed,ambientSeed,lampSeed the pixel's own key, offset once per question and once per
///        walk. Three because the three draws must not move together, and one set per walk because
///        a cloud and a rainstorm over one pixel are two points in the world — `SEED_MEDIUM_SUN`
///        says what seeding them alike would draw.
PuffAbove standsOverPuffs(vec3 point, uint sunSeed, uint ambientSeed, uint lampSeed)
{
    PuffAbove above;
    above.mSunLit = 1.0;

    if (sunUp())
    {
        uint state = randomSeed(sunSeed);
        const vec2 draw = vec2(randomNext(state), randomNext(state));

        above.mSunLit
            = lightThrough(point, coneDirection(frame.mSunPosition, sin(SUN_SHADOW_RADIUS), draw), frame.mFar);
    }

    above.mAmbientLit
        = ambientReaching(point, skyLights() ? vec3(0.0, 0.0, 1.0) : vec3(0.0), vec3(0.0), 0.0, ambientSeed);

    // **The lamp that matters where the layer starts, and its answer for all of them.** The
    // reservoir picks by what a lamp delivers here, so the one traced to is the one the layer would
    // most notice the loss of.
    uint lampState = randomSeed(lampSeed);
    const Reservoir kept = lampsInMedium(lampState, point);

    above.mLampLit = lampVisible(kept, vec2(randomNext(lampState), randomNext(lampState)));
    above.mLampToward = kept.mTowards;

    return above;
}

/// What a sprite's own texture lets through to a point on it from `toward`, out of its six-way bake.
///
/// **Six directions, weighted by how much of `toward` lies along each and divided by the same
/// weights**, so that a texel nothing shadows is lit in full from anywhere. The four in the sprite's
/// plane are `Rtx::SpriteLightMap`'s channels, read in the order it wrote them; the two out of the
/// plane are derived here, the way that class says: light from the front reaches the visible
/// surface whole, and light from behind crosses the texel's own thickness, which is `back`.
///
/// @param toward unit, from the sprite toward the light — or zero, for a light that is not there,
///        which is lit in full because nothing is being asked.
/// @param planeAcross,planeUp the sprite's own `u` and `v` in the world, which for a disc are the
///        screen's.
/// @param facing where the eye is, unit, from the sprite.
/// @param shade the bake at this texel, already thinned by the sprite's own fade.
float sixWayThrough(vec3 toward, vec3 planeAcross, vec3 planeUp, vec3 facing, vec4 shade, float back)
{
    const vec3 along = vec3(dot(toward, planeAcross), dot(toward, planeUp), dot(toward, facing));
    const vec3 positive = max(along, vec3(0.0));
    const vec3 negative = max(-along, vec3(0.0));

    const float weight = dot(positive + negative, vec3(1.0));
    if (!(weight > 0.0))
        return 1.0;

    return (positive.x * shade.x + negative.x * shade.y + positive.y * shade.z + negative.y * shade.w + positive.z
               + negative.z * back)
        / weight;
}

/// One puff's case for owning a pixel's motion vector.
struct PuffClaim
{
    /// Where the eye stood relative to the puff.
    vec3 mToward;

    /// How far it travelled since the last frame, in world units.
    vec3 mMoved;

    /// How strong the case is, in whatever its kind is judged by — the share it hid, or the light
    /// it added. Nought for a claim nothing filled.
    float mWeight;
};

/// What the puffs between the eye and a surface add to the frame, and what they leave of it.
///
/// **Two walks fill one of these**, because the two are the same kind of thing: `spritesAlong`
/// gathers what an emitter drew, and `mediumAlong` gathers the shells of a cloud the content
/// modelled as geometry. `mergedPuffs` is what puts the two together, and everything downstream —
/// the air split, the composite, the claim, the layer the upscaler is handed — reads one layer and
/// never asks which walk filled it.
struct PuffLayer
{
    /// What the covering puffs look like where they cover a pixel whole — a straight colour and
    /// not one premultiplied by `1 - mTransmittance`.
    ///
    /// **Straight, because that is what a colour is**, and because the two composites want it that
    /// way at different moments: the frame's own multiplies it by the coverage where it composites,
    /// and the layer handed to Ray Reconstruction is premultiplied where it is written.
    /// `visibility.rgen` measured which the upscaler takes.
    ///
    /// Already fog-attenuated where it stands, so a caller composites this over a frame the fog has
    /// finished with rather than putting it through the fog a second time.
    vec3 mColour;

    /// What the additive sprites put in, which no coverage carries.
    ///
    /// **Apart from `mColour`, because an alpha blend cannot express a flame.** A layer handed to
    /// the upscaler is a colour and an opacity, and a flame's opacity is nought by definition — so
    /// what it adds rides with the frame behind it instead. What that costs is that a plume across
    /// a flame now dims it, which is the one case this walk's own note says it did not model.
    vec3 mAdded;

    /// What survives the covering puffs. One for a frame with only flames in it: additive
    /// blending hides nothing by definition.
    float mTransmittance;

    /// How far along the ray the covering happened, weighted by how much each sprite covered.
    ///
    /// **What the caller splits the air at.** A layer that covers what is behind it must not cover
    /// the haze in front of it, and the mean is the right depth for the same reason the mean colour
    /// is the right colour: the walk has no order to composite by, so it reports what the coverage
    /// came to and where it came from. Nought for a frame that covered nothing.
    float mCoveredAt;

    /// The covering puff that hid the most of this pixel, and the additive sprite that put the most
    /// light into it.
    ///
    /// **Two, because the two kinds of blending are two different ways to own a pixel** and there is
    /// no single number that ranks them against each other. Smoke owns by covering: an unlit puff
    /// contributes no light at all and still decides everything the pixel shows, because what it
    /// shows is the puff. A flame owns by outshining: it hides nothing by definition, so no measure
    /// of coverage will ever find it. `puffClaim` is where the two are told apart.
    ///
    /// One of them wins in the end, because a pixel gets one motion vector and blending two
    /// velocities gives a third that describes neither.
    PuffClaim mCovering;
    PuffClaim mAdding;
};

/// A layer with nothing in it, which is what both walks start from and what either answers with
/// where it found nothing.
PuffLayer noPuffs()
{
    PuffLayer layer;
    layer.mColour = vec3(0.0);
    layer.mAdded = vec3(0.0);
    layer.mTransmittance = 1.0;
    layer.mCoveredAt = 0.0;
    layer.mCovering = PuffClaim(vec3(0.0), vec3(0.0), 0.0);
    layer.mAdding = PuffClaim(vec3(0.0), vec3(0.0), 0.0);

    return layer;
}

/// How much of the sprite's rim the mip chain has already eaten, as a factor to taper it by.
///
/// **A sprite a few pixels across is sampled several levels down its own chain**, by which point the
/// blob the artist painted has been averaged into a nearly flat wash and the only shape left is the
/// square the texture was cut to — so a spark at any distance reads as a little rectangle. The
/// silhouette has to be put back geometrically, and only where it was lost: none at the top level,
/// all of it two levels down, where a four-by-four block has already become one texel. Applied
/// everywhere instead it tapers a sprite twice and the fire visibly dims.
///
/// It costs nothing that was painted — every particle texture the game ships is a blob on a
/// transparent border, so the rim it removes held nothing.
float spriteTaper(float radial, float lod)
{
    // Written the way round GLSL defines: `smoothstep` with its first edge above its second is
    // undefined, however reliably it happens to produce the descending ramp.
    return mix(1.0, 1.0 - smoothstep(0.6, 1.0, radial), clamp(0.5 * lod, 0.0, 1.0));
}

/// Every emitter's sprites the ray crosses, composited.
///
/// **No acceleration structure and one sphere per emitter.** A lamp is asked for by a shading
/// *point*, which the uniform grid answers in a lookup; an emitter is asked for by a whole *ray*,
/// which would have to walk that grid cell by cell. There are tens of emitters in a cell and each
/// is small, so one rejection throws an emitter away for almost every pixel of the frame.
///
/// **Order-independent, because there is no order to be had.** `osgParticle` keeps its array in
/// birth order and sorting tens of sprites per pixel is not affordable. So the two kinds are
/// composited by what each actually means rather than by depth. The covering ones accumulate an
/// exact total coverage `1 - prod(1 - a)` and fill it with their own coverage-weighted mean colour:
/// exact for one sprite and for any number of sprites of one colour, which is what a single
/// emitter's smoke is, and it degrades to a blend rather than to a fault when they differ. The
/// adding ones accumulate a screen, `1 - prod(1 - e)` per channel — what a stack of things that
/// emit and absorb alike comes to, order-free, and the smooth form of the clamp the original's
/// framebuffer put on the same sum: a fire's core saturates at a lit surface's white rather than
/// piling twenty quads into a hundred times one.
///
/// What it does not model is a covering sprite in front of an adding one — a plume across a flame
/// would dim it, and here it does not. The two are separate emitters in Morrowind's content and
/// they are stacked rather than crossed.
PuffLayer spritesAlong(uvec2 pixel, vec3 origin, vec3 direction, float limit)
{
    PuffLayer layer = noPuffs();

    vec3 covered = vec3(0.0);
    float coverage = 0.0;
    float coveredAt = 0.0;
    vec3 addedThrough = vec3(1.0);

    // The screen's own axes, for reading a sprite's texture the way the quad would have been cut,
    // and the cone a pixel of it covers. Hoisted because they are the camera's and not the sprite's.
    const vec3 across = normalize(frame.mCamera.mRight);
    const vec3 upward = normalize(frame.mCamera.mUp);
    const Cone cone = coneAt(frame.mCamera);

    // **The tiles are derived and not carried**, from the same function the bin uses, so the two
    // cannot disagree about how many there are across.
    const uint tile = (pixel.y / SPRITE_TILE) * spriteTilesOver(frame.mCamera.mWidth) + pixel.x / SPRITE_TILE;

    // **Every sprite where the runs did not fit**, which is the list's own degenerate form and the
    // march as it was before the tiles: `SPRITE_LIST_UNBINNED` says when a frame is handed it. The
    // run is then every index in turn, so a slot names its sprite directly.
    const bool unbinned = spriteTileListAt(0u) == SPRITE_LIST_UNBINNED;
    uint slot = unbinned ? 0u : spriteTileListAt(tile);
    const uint last = unbinned ? spriteTileListAt(1u) : spriteTileListAt(tile + 1u);

    // **What the outer loop over emitters used to hold, carried across a walk that no longer has
    // one.** The tile's sprites are in ascending index, and a sprite's index is contiguous within
    // its emitter, so an emitter's sprites arrive consecutively and these are worked out once for
    // each run — which is the amortisation the emitter loop was giving away for free.
    uint held = ~0u;
    GpuEmitter emitter;
    bool missed = true;
    float extinction = 0.0;
    bool oriented = false;
    float width = 0.0;
    float widest = 0.0;
    vec3 lamps = vec3(0.0);

    // What one layer of this emitter's texture hides on average, read from its coarsest level.
    //
    // **Read at the first sprite that wants it rather than at the emitter**, because whether any
    // does is a property of the sprite: `mSunLayers` counts what stands between *this* one and the
    // sun, so an emitter's outermost sprites carry nothing and would pay two texture reads for an
    // answer they never look at. Negative until read, which no alpha can be.
    float layerMean = -1.0;

    // **What stands over the layer, asked once for it and not once for a puff** —
    // `standsOverPuffs` says what the three rays are and why they are shared with the medium's own
    // walk. Taken at the first sprite that is lit, which is the nearest one and so the one whose
    // light most of what the pixel shows is composited from.
    bool askedAbove = false;
    PuffAbove above = PuffAbove(1.0, 1.0, 1.0, vec3(0.0));

    // The pixel's own key, hashed once for the three draws above and the one below.
    const uint key = pixelKey(pixel);

    const vec3 toSun = frame.mSunPosition;
    const vec3 skyward = vec3(0.0, 0.0, 1.0);

    // **The sun's share thrown forward, which is one angle for the whole ray.** A directional source
    // holds its angle to a straight ray, so the phase function is one evaluation for every sprite on
    // it — the same argument `fogSourcesAlong` makes, and the reason a shape this costly is
    // affordable at all.
    //
    // The light travels `-toSun` and what the eye catches travels `-direction`, so the cosine
    // between them is this dot. Taken as the ratio to the even share, because `puffLight` gives a
    // puff a card's worth of the sun rather than a sphere's.
    const float thrownForward = henyeyGreenstein(SMOKE_ANISOTROPY, dot(toSun, direction)) / INV_FOUR_PI;

    for (; slot < last; ++slot)
    {
        const GpuSprite sprite = spriteAt(unbinned ? slot : spriteTileListAt(slot));

        if (sprite.mEmitter != held)
        {
            held = sprite.mEmitter;
            emitter = emitterAt(held);
            layerMean = -1.0;

            const vec3 toCentre = emitter.mCentre - origin;
            const float along = dot(toCentre, direction);

            // The same two rejections the emitter loop made, kept because a tile is sixteen pixels
            // wide and a sprite in it is one *some* ray of the tile can reach rather than this one.
            missed = along + emitter.mReach <= 0.0 || along - emitter.mReach >= limit
                || dot(toCentre, toCentre) - along * along > emitter.mReach * emitter.mReach;

            if (!missed)
            {
                // **One evaluation of the fog's field for the whole emitter**, taken halfway to it:
                // that is the mean-value point of the path, and the field costs forty hashes out of
                // doors. Every sprite behind this sphere is within `mReach` of the same air.
                extinction = fogExtinctionAt(origin + direction * (0.5 * along), max(along, 1.0));

                // **And one walk of the cell's lamps for it, taken where the ray passes nearest.**
                // A storm is one emitter of a couple of thousand drops, so this ran once for every
                // drop over every pixel it covered — tens of walks of a town's four hundred lamps
                // per pixel. Over Balmora at night in the rain it takes the trace from 6.07 ms to
                // 4.24, which is the largest single saving in this file.
                //
                // The cost in the picture is that a lamp's falloff stops varying across one
                // emitter's own reach, where its shadow answer already did not vary across the
                // whole layer. Beside a lit lantern in the rain — the frame built to expose it —
                // the worst pixel moves 3.7% and the 99th moves 0.15%.
                //
                // The closest approach and not the midpoint the air takes: what is wanted here is
                // where the sprites are, and the air's answer is an integral along the path.
                //
                // **And not clamped to `limit` either, which was tried.** A sphere can reach past
                // the surface the ray stops at, and holding the sample at that surface reads as the
                // safer answer — but the sprites are scattered about the *centre* rather than along
                // the ray, so the closest approach is the nearer point to them in three dimensions.
                // Clamping doubled the pixels that disagree with a per-sprite sum in Vivec's
                // canalworks, which is the one place in the corpus that has the case.
                lamps = lampsAt(origin + direction * max(along, 0.0));

                // **Two zero axes is a sprite that faces the eye**, which is nearly every emitter in
                // the game; asked once for the emitter rather than once for each of its sprites.
                // `fixed` is a reserved word in GLSL, which is why this is not called one.
                oriented
                    = dot(emitter.mAcross, emitter.mAcross) > 0.0 && dot(emitter.mUpward, emitter.mUpward) > 0.0;

                // How wide the streak is against how long, which is the shape the content authored
                // and the one thing kept from its across axis. Asked here for the same reason.
                width = oriented ? length(emitter.mAcross) : 0.0;

                // The texture's own extent, which every sprite of this emitter shares. Only the
                // wider axis reaches the level below.
                const vec2 size = vec2(textureSize(textures[nonuniformEXT(emitter.mTexture)], 0));
                widest = max(size.x, size.y);
            }
        }

        if (missed)
            continue;

        const vec3 toSprite = sprite.mPosition - origin;

        // How far along the ray the eye sees the sprite, what share of the sprite's own depth that
        // is, where across it the ray crossed in units of its own half-extents, how far out that is
        // as a fraction, and how far the sprite's surface stands toward the eye there — the things
        // the rest needs, and the only place the two kinds of sprite differ.
        float seen;
        float fraction;
        vec2 at;
        float radial;
        float lift = 0.0;

        if (oriented)
        {
            // **A quad that hangs in the world**, so the ray meets a plane rather than a ball. A
            // rain streak is a thin thing, and where it meets a wall is where the drop does.
            //
            // **The axis it hangs on is the content's; which way its width faces is not.**
            // `osgParticle` uses both authored axes untransformed for a `FIXED` system because
            // a rasterizer has to commit the quad to some plane, and Morrowind's rain commits
            // it to the world's X–Z one — so a drop looked at from along X is a polygon seen
            // edge-on and thins away to nothing, and the same storm reads three times heavier
            // facing north than facing east. That is a fact about drawing quads rather than
            // about rain, and it is the sort of thing rays are here to stop answering with.
            //
            // So the streak's axis is kept exactly as authored — its length, its fall, the lean
            // the wind gave it — and only the width is swung about that axis to meet the ray.
            // Seen face-on, which is where the content was authored and judged, nothing moves.
            const vec3 axis = emitter.mUpward;
            const vec3 swung = cross(axis, direction);
            const float swing = length(swung);

            // Looking straight down the streak's own axis, where no swing presents any width.
            // There is nothing to see from there either, so the authored width stands in.
            const vec3 side = swing > 1.0e-4 ? swung * (width / swing) : emitter.mAcross;

            const vec3 quadAcross = side * sprite.mRadius;
            const vec3 quadUpward = axis * sprite.mRadius;
            const vec3 normal = cross(quadAcross, quadUpward);

            const float facing = dot(normal, direction);
            if (abs(facing) <= 1.0e-6)
                continue;

            const float depth = dot(toSprite, normal) / facing;
            if (depth <= 0.0 || depth >= limit)
                continue;

            const vec3 offset = direction * depth - toSprite;
            at = vec2(dot(offset, quadAcross) / dot(quadAcross, quadAcross),
                dot(offset, quadUpward) / dot(quadUpward, quadUpward));
            if (max(abs(at.x), abs(at.y)) >= 1.0)
                continue;

            radial = length(at);
            seen = depth;
            fraction = 1.0;
        }
        else
        {
            // **A ball and not a disc, and what the eye sees of it is a chord.** The disc the
            // rasterizer drew is the ball's silhouette, and everything it painted is kept: in the
            // open the chord is whole and the sprite composites exactly as the quad did. What the
            // ball adds is an inside, so where it runs into a wall — or the wall into it, or the eye
            // into either — the chord is cut at the surface and the sprite fades along the ray
            // instead of being clipped at its centre. That is the spherical billboard, and it is the
            // exact form of what a rasterizer's soft particle approximates with a depth fade.
            //
            // Perpendicular to the ray rather than to the camera's axis, so a sprite at the corner
            // of the frame faces the eye and not the screen.
            const float depth = dot(toSprite, direction);
            const vec3 offset = toSprite - direction * depth;
            const float radial2 = dot(offset, offset) / (sprite.mRadius * sprite.mRadius);
            if (radial2 >= 1.0)
                continue;

            lift = sqrt(1.0 - radial2);
            const float halfChord = sprite.mRadius * lift;
            const float from = max(depth - halfChord, 0.0);
            const float until = min(depth + halfChord, limit);
            if (until <= from)
                continue;

            fraction = (until - from) / (2.0 * halfChord);
            seen = 0.5 * (from + until);
            radial = sqrt(radial2);
            at = -vec2(dot(offset, across), dot(offset, upward)) / sprite.mRadius;
        }

        // The sprite is `2 * mRadius` wide where the pixel's cone has spread to `mWidth + mSpread *
        // seen`, and the ratio of the two in texels is the level that resolves it. Clamped inside
        // the logarithm rather than outside, because an eye inside the ball sees it at no distance.
        // `coneAt` and not `mSpreadAngle`, for the reason it gives: a map tile's cone never widens
        // and is a pixel of the box wide from the start, where the angle alone read every sprite in
        // it at level zero.
        const float lod
            = log2(max(widest * (cone.mWidth + cone.mSpread * seen) / (2.0 * sprite.mRadius), 1.0));

        // The quad `osgParticle` would have drawn: texture coordinate zero at `-right -up` and
        // one at `+right +up`, about a centre at half.
        const vec2 uv = at * 0.5 + 0.5;

        const vec4 texel = textureLod(textures[nonuniformEXT(emitter.mTexture)], uv, lod);

        // **The rim is put back on a disc and left alone on a quad.** What the taper restores is
        // a round blob the mip chain averaged into the square it was cut to; a rain streak was
        // authored as that rectangle, and tapering it would round off the drop.
        const float painted = texel.a * sprite.mAlpha * (oriented ? 1.0 : spriteTaper(radial, lod));
        if (!(painted > 0.0))
            continue;

        // What the eye's share of the chord hides, which is `paintedOver`'s own law: the whole of
        // what was painted for a whole chord, and less for part of one.
        const float alpha = paintedOver(painted, fraction);
        const vec3 colour = texel.rgb * sprite.mColour;
        const float reaching = exp(-extinction * seen);

        if (emitter.mAdditive != 0u)
        {
            // **No gain, deliberately.** The blend the file asks for says exactly how much light
            // the sprite adds; `FLAME_INTENSITY` is only what carries the original's scale, where
            // a fully lit surface reached one, onto this renderer's. A flame then comes out tens of
            // times the mean of the room it stands in, because that is what a flame is, and the
            // exposure downstream decides where it lands. A gain on top of it blows every flame to
            // a white square and hides the shape that was already in the texture.
            //

            // **And it absorbs as much as it emits, per channel**, which is what makes the screen
            // in the accumulator exact for a stack of them: what one sprite adds is what it always
            // added, and what twenty add saturates at the white the original's framebuffer clamped
            // to, rather than at twenty times it. The chord cuts a flame at a log the way it cuts
            // smoke at a wall.
            const vec3 glow = paintedOver(colour * painted, fraction) * reaching;
            addedThrough *= 1.0 - glow;

            const float lit = dot(glow, LUMINANCE_WEIGHTS) * FLAME_INTENSITY;

            if (lit > layer.mAdding.mWeight)
                layer.mAdding = PuffClaim(direction * seen, sprite.mMoved, lit);

            continue;
        }

        if (!askedAbove)
        {
            askedAbove = true;
            above = standsOverPuffs(sprite.mPosition, key + SEED_SPRITE_SUN, key + SEED_AMBIENT_REACHING,
                key + SEED_LAMPS_SPRITE);
        }

        // What this puff is lit by, over what the layer is: the ball's own side, and what its
        // texture lets through to this texel. A quad hanging in the world keeps the layer's answers
        // — a rain streak is a thin thing seen by what passes through it, and has no side.
        float sunLit = above.mSunLit;
        float skyLit = above.mAmbientLit;
        float lampLit = above.mLampLit;
        float fillLit = above.mAmbientLit;

        if (!oriented)
        {
            // Where the ray entered the ball, as a normal: `at` across the disc, and the ball's
            // surface lifted toward the eye by what is left of the radius there.
            const vec3 normal = normalize(across * at.x + upward * at.y - direction * lift);

            // `SMOKE_ANISOTROPY` says why the sky and the lamps below are not thrown.
            sunLit *= thrownForward;

            sunLit *= ballWrap(normal, toSun);

            skyLit *= ballWrap(normal, skyward);
            lampLit *= ballWrap(normal, above.mLampToward);

            if (emitter.mLighting != NO_TEXTURE)
            {
                // **What the puff's own texture leaves of each light**, thinned as the puff's own
                // fade thins it: a wisp near the end of its life shadows itself less than the puff
                // it was. The power is the same one the bake takes per texel, applied to the whole.
                const vec4 shade
                    = pow(textureLod(textures[nonuniformEXT(emitter.mLighting)], uv, lod), vec4(sprite.mAlpha));
                const float back = 1.0 - texel.a * sprite.mAlpha;
                const vec3 facing = -direction;

                sunLit *= sixWayThrough(toSun, across, upward, facing, shade, back);
                skyLit *= sixWayThrough(skyward, across, upward, facing, shade, back);
                lampLit *= sixWayThrough(above.mLampToward, across, upward, facing, shade, back);
                fillLit *= (shade.x + shade.y + shade.z + shade.w + 1.0 + back) / 6.0;
            }

            // **What the rest of its own emitter leaves of the light**, as the layers of sprites
            // between this one and the sun and the sky — counted on the host by `Rtx::SpriteShade`
            // — thinned here by what one layer of this texture hides on average, which is its
            // coarsest level. The limit keeps an opaque texture from shutting the light outright.
            if (sprite.mSunLayers > 0.0 || sprite.mSkyLayers > 0.0)
            {
                if (layerMean < 0.0)
                {
                    const float coarsest = float(textureQueryLevels(textures[nonuniformEXT(emitter.mTexture)]) - 1);
                    layerMean = textureLod(textures[nonuniformEXT(emitter.mTexture)], vec2(0.5), coarsest).a;
                }

                const float layer = 1.0 - min(layerMean, SPRITE_ALPHA_LIMIT);

                sunLit *= pow(layer, sprite.mSunLayers);
                skyLit *= pow(layer, sprite.mSkyLayers);
            }
        }


        const vec3 daylight = daylightReaching(sprite.mPosition);

        covered += colour * puffLight(daylight, lamps, sunLit, skyLit, fillLit, lampLit) * (alpha * reaching);
        coverage += alpha;
        coveredAt += seen * alpha;
        layer.mTransmittance *= 1.0 - alpha;

        // **By what it hid and not by what it was lit by.** An unlit puff of smoke sends back no
        // light at all and still decides the whole of what the pixel shows.
        if (alpha > layer.mCovering.mWeight)
            layer.mCovering = PuffClaim(direction * seen, sprite.mMoved, alpha);
    }

    if (coverage > 0.0)
    {
        layer.mColour = covered / coverage;
        layer.mCoveredAt = coveredAt / coverage;
    }

    layer.mAdded = (1.0 - addedThrough) * FLAME_INTENSITY;


    return layer;
}

/// Which sprite over a pixel owns its motion vector, if any of them does.
///
/// **Two ways to own a pixel, because there are two ways to blend into one.** A covering sprite owns
/// it by hiding most of what was behind: `1 - mTransmittance` is exactly the share of the pixel that
/// is the sprite rather than the surface, whatever either of them is lit by, and past a half the
/// majority of what the pixel shows is the sprite. An additive one hides nothing by definition — no
/// measure of coverage will ever find a flame — and owns the pixel instead when what it added
/// outshines what the layer left of the surface behind it.
///
/// Neither clause subsumes the other. An unlit puff of smoke contributes no light and still decides
/// everything the pixel shows; a flame over a dark wall contributes all of it and covers nothing.
///
/// @param behind the frame as it stood before the sprites were composited over it.
/// @return a claim whose `mWeight` is nought where the surface keeps its own pixel.
PuffClaim puffClaim(vec3 behind, PuffLayer layer)
{
    if (layer.mTransmittance < 0.5)
        return layer.mCovering;

    const float left = dot(behind, LUMINANCE_WEIGHTS) * layer.mTransmittance;
    if (layer.mAdding.mWeight > left)
        return layer.mAdding;

    return PuffClaim(vec3(0.0), vec3(0.0), 0.0);
}

/// Two layers of puffs as one.
///
/// **The same rule each walk already uses inside itself, applied once more.** Neither walk has an
/// order to composite by, so each reports the exact coverage `1 - prod(1 - a)` filled with its own
/// coverage-weighted mean colour and taken at its own coverage-weighted depth. Putting two of those
/// together is the same arithmetic on two terms instead of many, and it is exact wherever the
/// colours agree — which is what one emitter's smoke and one cloud's shells each are.
///
/// **What it gives up is the depth**, and only where both walks found something on one pixel: rain
/// a few units out and a cloud two thousand away come to one mean the air is split at. The weight
/// is the coverage, so the one the pixel mostly shows is the one the split is right for.
PuffLayer mergedPuffs(PuffLayer first, PuffLayer second)
{
    const float firstCoverage = 1.0 - first.mTransmittance;
    const float secondCoverage = 1.0 - second.mTransmittance;
    const float coverage = firstCoverage + secondCoverage;

    PuffLayer layer;
    layer.mAdded = first.mAdded + second.mAdded;
    layer.mTransmittance = first.mTransmittance * second.mTransmittance;
    layer.mColour = coverage > 0.0
        ? (first.mColour * firstCoverage + second.mColour * secondCoverage) / coverage
        : vec3(0.0);
    layer.mCoveredAt
        = coverage > 0.0 ? (first.mCoveredAt * firstCoverage + second.mCoveredAt * secondCoverage) / coverage : 0.0;

    // **The stronger case wins outright rather than being blended into the other.** A pixel gets one
    // motion vector, and a velocity halfway between a raindrop's and a still cloud's describes
    // neither.
    layer.mCovering = first.mCovering.mWeight >= second.mCovering.mWeight ? first.mCovering : second.mCovering;
    layer.mAdding = first.mAdding.mWeight >= second.mAdding.mWeight ? first.mAdding : second.mAdding;

    return layer;
}

#endif
