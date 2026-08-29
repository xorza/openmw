// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_SPRITES_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_SPRITES_GLSL

// The particle layer, marched against the primary ray rather than built into an
// acceleration structure — and which sprite over a pixel owns its motion.

#include "colour.h"
#include "scene.h"
#include "bindings.glsl"
#include "variants.glsl"
#include "fog.glsl"
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
/// same `lampsAt` and the same one multiply on the sum.
///
/// **A particle has no normal and it still has an up**, which is how a sprite is occluded by the
/// world: what a point sees of the sky is a question about the point, and the sky is above. What
/// it has instead of a normal is a side — `ballWrap` — and a thickness its own texture records —
/// `sixWayThrough` — and those arrive folded into the factors below. `spritesAlong` says why the
/// world's part of them is asked once for a layer rather than once for a puff.
///
/// @param daylight what `daylightReaching` says of the position, asked once by the caller.
/// @param sunLit what the world and the puff itself leave of the sun at this point.
/// @param skyLit the same for the sky over it.
/// @param fillLit what the puff lets through of a room's own fill, which comes from everywhere.
/// @param lampLit the same for the lamps — **one shadow answer for every lamp and for the whole
///        layer**, where the sum beside it is this puff's own. What a lamp delivers runs as one over
///        the square of a distance that changes from sprite to sprite; whether it is *seen* changes
///        slowly, and a torch behind a wall is behind it for the whole layer.
vec3 puffLight(vec3 position, vec3 daylight, float sunLit, float skyLit, float fillLit, float lampLit)

{
    // `pathEnd`, with the sky's share and the room's share of the ambient each shadowed by its own
    // answer rather than the room's by none: a fill comes from everywhere, so what a puff lets
    // through of it is the mean of every way in.
    return frame.mAmbient * (daylight * mix(fillLit, skyLit, frame.mAmbientFromSky))
        + (HAS_SUN ? frame.mSunIrradiance * (INV_PI * daylight * sunLit) : vec3(0.0))
        + INV_FOUR_PI * lampsAt(position) * lampLit;
}

/// How a ball is lit from `toward` against its mean, on the side of it the eye sees.
///
/// A zero `toward` — a lamp that is not there — is lit at the mean, because nothing is being asked.
float ballWrap(vec3 normal, vec3 toward)
{
    return 1.0 + SPRITE_WRAP * dot(normal, toward);
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

/// One sprite's case for owning a pixel's motion vector.
struct SpriteClaim
{
    /// Where the eye stood relative to the sprite.
    vec3 mToward;

    /// How far it travelled since the last frame, in world units.
    vec3 mMoved;

    /// How strong the case is, in whatever its kind is judged by — the share it hid, or the light
    /// it added. Nought for a claim nothing filled.
    float mWeight;
};

/// What the sprites between the eye and `limit` add to the frame, and what they leave of it.
struct SpriteLayer
{
    /// Already fog-attenuated per sprite, so a caller composites this over a frame the fog has
    /// finished with rather than putting it through the fog a second time.
    vec3 mRadiance;

    /// What survives the covering sprites. One for a frame with only flames in it: additive
    /// blending hides nothing by definition.
    float mTransmittance;

    /// The covering sprite that hid the most of this pixel, and the additive one that put the most
    /// light into it.
    ///
    /// **Two, because the two kinds of blending are two different ways to own a pixel** and there is
    /// no single number that ranks them against each other. Smoke owns by covering: an unlit puff
    /// contributes no light at all and still decides everything the pixel shows, because what it
    /// shows is the puff. A flame owns by outshining: it hides nothing by definition, so no measure
    /// of coverage will ever find it. `spriteClaim` is where the two are told apart.
    ///
    /// One of them wins in the end, because a pixel gets one motion vector and blending two
    /// velocities gives a third that describes neither.
    SpriteClaim mCovering;
    SpriteClaim mAdding;
};

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
SpriteLayer spritesAlong(uvec2 pixel, vec3 origin, vec3 direction, float limit)
{
    SpriteLayer layer;
    layer.mRadiance = vec3(0.0);
    layer.mTransmittance = 1.0;
    layer.mCovering = SpriteClaim(vec3(0.0), vec3(0.0), 0.0);
    layer.mAdding = SpriteClaim(vec3(0.0), vec3(0.0), 0.0);

    vec3 covered = vec3(0.0);
    float coverage = 0.0;
    vec3 addedThrough = vec3(1.0);

    // The screen's own axes, for reading a sprite's texture the way the quad would have been cut.
    // Hoisted because they are the camera's and not the sprite's.
    const vec3 across = normalize(frame.mCamera.mRight);
    const vec3 upward = normalize(frame.mCamera.mUp);

    // **The tiles are derived and not carried**, from the same expression `Rtx::SpriteTiles` uses,
    // so the two cannot disagree about how many there are across.
    const uint tilesAcross = (frame.mCamera.mWidth + SPRITE_TILE - 1u) / SPRITE_TILE;
    const uint tile = (pixel.y / SPRITE_TILE) * tilesAcross + pixel.x / SPRITE_TILE;
    const uint last = spriteTileOffsets[tile + 1u];

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

    // **What stands over the layer, asked once for it and not once for a puff.** A shadow ray inside
    // the loop below would multiply with however many sprites a rainstorm puts over a pixel, which
    // is the reason this was unshadowed at all. Two rays serve the whole layer instead, taken at the
    // first sprite that is lit — the nearest one, and so the one whose light most of what the pixel
    // shows is composited from.
    //
    // The sun's is skipped where there is no sun, and the sky's where the ambient is a room's own
    // fill rather than the sky — which `skyReaching` decides for itself.
    //
    // The lamp the layer traced to is also the one it is given a side toward: the reservoir picks
    // by what a lamp delivers here, so it is the one the layer would most notice the loss of.
    bool askedAbove = false;
    float sunThrough = 1.0;
    float skyThrough = 1.0;
    float lampThrough = 1.0;
    vec3 lampToward = vec3(0.0);

    const vec3 toSun = frame.mSunPosition;
    const vec3 skyward = vec3(0.0, 0.0, 1.0);

    for (uint slot = spriteTileOffsets[tile]; slot < last; ++slot)
    {
        const GpuSprite sprite = sprites[spriteTileIndices[slot]];

        if (sprite.mEmitter != held)
        {
            held = sprite.mEmitter;
            emitter = emitters[held];

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

                // **Two zero axes is a sprite that faces the eye**, which is nearly every emitter in
                // the game; asked once for the emitter rather than once for each of its sprites.
                // `fixed` is a reserved word in GLSL, which is why this is not called one.
                oriented
                    = dot(emitter.mAcross, emitter.mAcross) > 0.0 && dot(emitter.mUpward, emitter.mUpward) > 0.0;

                // How wide the streak is against how long, which is the shape the content authored
                // and the one thing kept from its across axis. Asked here for the same reason.
                width = oriented ? length(emitter.mAcross) : 0.0;
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

        // The sprite is `2 * mRadius` wide where the pixel's cone is `mSpreadAngle * seen` across,
        // and the ratio of the two in texels is the level that resolves it. Clamped inside the
        // logarithm rather than outside, because an eye inside the ball sees it at no distance.
        const vec2 size = vec2(textureSize(textures[nonuniformEXT(emitter.mTexture)], 0));
        const float lod = log2(max(max(size.x, size.y) * frame.mCamera.mSpreadAngle * seen / (2.0 * sprite.mRadius), 1.0));

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

        // What the eye's share of the chord hides: exactly what was painted for a whole chord, and
        // `(1 - painted) ^ fraction` of the background let through for part of one.
        // `SPRITE_ALPHA_LIMIT` says why the power is not taken at one.
        const float alpha = 1.0 - pow(1.0 - min(painted, SPRITE_ALPHA_LIMIT), fraction);
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
            const vec3 glow
                = (1.0 - pow(1.0 - min(colour * painted, vec3(SPRITE_ALPHA_LIMIT)), vec3(fraction))) * reaching;
            addedThrough *= 1.0 - glow;

            const float lit = dot(glow, LUMINANCE_WEIGHTS) * FLAME_INTENSITY;

            if (lit > layer.mAdding.mWeight)
                layer.mAdding = SpriteClaim(direction * seen, sprite.mMoved, lit);

            continue;
        }

        if (!askedAbove)
        {
            askedAbove = true;

            if (HAS_SUN && frame.mSunIrradiance != vec3(0.0))
            {
                uint state = randomSeed(pixelKey(pixel) + SEED_SPRITE_SUN);
                const vec2 draw = vec2(randomNext(state), randomNext(state));

                sunThrough = lightThrough(sprite.mPosition,
                    coneDirection(frame.mSunPosition, sin(SUN_SHADOW_RADIUS), draw), frame.mFar);

            }

            // Straight up, because a particle has no normal and the sky is above it either way.
            skyThrough = skyReaching(sprite.mPosition, skyward, 0.0, pixelKey(pixel) + SEED_SKY_REACHING);

            // **The lamp that matters where the layer starts, and its answer for all of them.** The
            // reservoir picks by what a lamp delivers here, so the one traced to is the one the
            // layer would most notice the loss of.
            uint lampState = randomSeed(pixelKey(pixel) + SEED_LAMPS_SPRITE);

            Reservoir lamps = noLamps();
            weighLamps(lamps, lampState, sprite.mPosition, vec3(0.0), INV_FOUR_PI, 0.0);

            lampThrough = lampVisible(lamps, vec2(randomNext(lampState), randomNext(lampState)));
            lampToward = lamps.mTowards;
        }

        // What this puff is lit by, over what the layer is: the ball's own side, and what its
        // texture lets through to this texel. A quad hanging in the world keeps the layer's answers
        // — a rain streak is a thin thing seen by what passes through it, and has no side.
        float sunLit = sunThrough;
        float skyLit = skyThrough;
        float lampLit = lampThrough;
        float fillLit = 1.0;

        if (!oriented)
        {
            // Where the ray entered the ball, as a normal: `at` across the disc, and the ball's
            // surface lifted toward the eye by what is left of the radius there.
            const vec3 normal = normalize(across * at.x + upward * at.y - direction * lift);

            // The sun's share thrown forward, as the ratio to the even share — the light travels
            // `-toSun` and what the eye catches travels `-direction`, so the cosine between them is
            // this dot. `SMOKE_ANISOTROPY` says why the sky and the lamps below are not thrown.
            sunLit *= henyeyGreenstein(SMOKE_ANISOTROPY, dot(toSun, direction)) / INV_FOUR_PI;

            sunLit *= ballWrap(normal, toSun);

            skyLit *= ballWrap(normal, skyward);
            lampLit *= ballWrap(normal, lampToward);

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
                lampLit *= sixWayThrough(lampToward, across, upward, facing, shade, back);
                fillLit = (shade.x + shade.y + shade.z + shade.w + 1.0 + back) / 6.0;
            }

            // **What the rest of its own emitter leaves of the light**, as the layers of sprites
            // between this one and the sun and the sky — counted on the host by `Rtx::SpriteShade`
            // — thinned here by what one layer of this texture hides on average, which is its
            // coarsest level. The limit keeps an opaque texture from shutting the light outright.
            if (sprite.mSunLayers > 0.0 || sprite.mSkyLayers > 0.0)
            {
                const float coarsest = float(textureQueryLevels(textures[nonuniformEXT(emitter.mTexture)]) - 1);
                const float mean = textureLod(textures[nonuniformEXT(emitter.mTexture)], vec2(0.5), coarsest).a;
                const float layer = 1.0 - min(mean, SPRITE_ALPHA_LIMIT);

                sunLit *= pow(layer, sprite.mSunLayers);
                skyLit *= pow(layer, sprite.mSkyLayers);
            }
        }


        const vec3 daylight = daylightReaching(sprite.mPosition);

        covered += colour * puffLight(sprite.mPosition, daylight, sunLit, skyLit, fillLit, lampLit) * (alpha * reaching);
        coverage += alpha;
        layer.mTransmittance *= 1.0 - alpha;

        // **By what it hid and not by what it was lit by.** An unlit puff of smoke sends back no
        // light at all and still decides the whole of what the pixel shows.
        if (alpha > layer.mCovering.mWeight)
            layer.mCovering = SpriteClaim(direction * seen, sprite.mMoved, alpha);
    }

    if (coverage > 0.0)
        layer.mRadiance += covered * ((1.0 - layer.mTransmittance) / coverage);

    layer.mRadiance += (1.0 - addedThrough) * FLAME_INTENSITY;


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
SpriteClaim spriteClaim(vec3 behind, SpriteLayer layer)
{
    if (layer.mTransmittance < 0.5)
        return layer.mCovering;

    const float left = dot(behind, LUMINANCE_WEIGHTS) * layer.mTransmittance;
    if (layer.mAdding.mWeight > left)
        return layer.mAdding;

    return SpriteClaim(vec3(0.0), vec3(0.0), 0.0);
}

#endif
