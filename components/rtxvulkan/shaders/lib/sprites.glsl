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
#include "lights.glsl"
#include "shading.glsl"

/// What a puff of smoke is lit by, per unit of albedo.
///
/// **As bright as a card of the same albedo held beside it, and that is not a fudge.** An opaque
/// diffuse *sphere* would catch `pi r^2` of the beam and radiate over `4 pi r^2` — a quarter of the
/// facing value — but a puff is neither opaque nor diffuse: it is a cloud of droplets that scatters
/// strongly forward and again inside itself, so the sun reaches all of it rather than one
/// hemisphere. The quarter, tried first in the reference implementation, put a plume back at the
/// sky's own ambient, where it was invisible.
///
/// Unshadowed, and the data is what makes that safe: `mSunIrradiance` is zero for a cell with no
/// sky, so a vent in a cave gets none of it without being asked.
///
/// The lamps arrive the way they arrive at the fog — as irradiance spread over the whole sphere —
/// because a puff is the same kind of thing the fog is, only denser and in one place. So it is the
/// same `lampsAt` and the same one multiply on the sum.
vec3 puffLight(vec3 position)
{
    return pathEnd(position) + frame.mSunIrradiance * (INV_PI * daylightReaching(position))
        + INV_FOUR_PI * lampsAt(position);
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
/// composited by what each actually means rather than by depth: an additive sprite adds light and
/// hides nothing, which is order-free outright; the covering ones accumulate an exact total
/// coverage `1 - prod(1 - a)` and fill it with their own coverage-weighted mean colour. That is
/// exact for one sprite and for any number of sprites of one colour, which is what a single
/// emitter's smoke is, and it degrades to a blend rather than to a fault when they differ.
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

        // How far along the ray the quad is, where across it the ray crossed in units of its own
        // half-extents, and how far out that is as a fraction — the three things the rest needs,
        // and the only place the two kinds of sprite differ.
        float depth;
        vec2 at;
        float radial;

        if (oriented)
        {
            // **A quad that hangs in the world**, so the ray meets a plane rather than a disc.
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

            const vec3 across = side * sprite.mRadius;
            const vec3 upward = axis * sprite.mRadius;
            const vec3 normal = cross(across, upward);

            const float facing = dot(normal, direction);
            if (abs(facing) <= 1.0e-6)
                continue;

            depth = dot(toSprite, normal) / facing;
            if (depth <= 0.0 || depth >= limit)
                continue;

            const vec3 offset = direction * depth - toSprite;
            at = vec2(dot(offset, across) / dot(across, across), dot(offset, upward) / dot(upward, upward));
            if (max(abs(at.x), abs(at.y)) >= 1.0)
                continue;

            radial = length(at);
        }
        else
        {
            depth = dot(toSprite, direction);
            if (depth <= 0.0 || depth >= limit)
                continue;

            // Perpendicular to the ray rather than to the camera's axis, so a sprite at the
            // corner of the frame faces the eye and not the screen.
            const vec3 offset = toSprite - direction * depth;
            radial = length(offset) / sprite.mRadius;
            if (radial >= 1.0)
                continue;

            at = -vec2(dot(offset, across), dot(offset, upward)) / sprite.mRadius;
        }

        // The sprite is `2 * mRadius` wide where the pixel's cone is `mSpreadAngle * depth`
        // across, and the ratio of the two in texels is the level that resolves it.
        const vec2 size = vec2(textureSize(textures[nonuniformEXT(emitter.mTexture)], 0));
        const float lod
            = max(0.0, log2(max(size.x, size.y) * frame.mCamera.mSpreadAngle * depth / (2.0 * sprite.mRadius)));

        // The quad `osgParticle` would have drawn: texture coordinate zero at `-right -up` and
        // one at `+right +up`, about a centre at half.
        const vec2 uv = at * 0.5 + 0.5;

        const vec4 texel = textureLod(textures[nonuniformEXT(emitter.mTexture)], uv, lod);

        // **The rim is put back on a disc and left alone on a quad.** What the taper restores is
        // a round blob the mip chain averaged into the square it was cut to; a rain streak was
        // authored as that rectangle, and tapering it would round off the drop.
        const float alpha
            = texel.a * sprite.mAlpha * (oriented ? 1.0 : spriteTaper(radial, lod));
        if (!(alpha > 0.0))
            continue;

        const vec3 colour = texel.rgb * sprite.mColour;
        const float reaching = exp(-extinction * depth);

        if (emitter.mAdditive != 0u)
        {
            // **No gain, deliberately.** The blend the file asks for says exactly how much light
            // the sprite adds; `EMISSIVE_INTENSITY` is only what carries the original's scale,
            // where a fully lit surface reached one, onto this renderer's, where the sun is
            // `DAYLIGHT`. A flame then comes out tens of times the mean of the room it stands in,
            // because that is what a flame is, and the exposure downstream decides where it
            // lands. A gain on top of it blows every flame to a white square and hides the shape
            // that was already in the texture.
            const vec3 added = colour * (alpha * reaching * EMISSIVE_INTENSITY);
            layer.mRadiance += added;

            const float lit = dot(added, LUMINANCE_WEIGHTS);
            if (lit > layer.mAdding.mWeight)
                layer.mAdding = SpriteClaim(direction * depth, sprite.mMoved, lit);

            continue;
        }

        covered += colour * puffLight(sprite.mPosition) * (alpha * reaching);
        coverage += alpha;
        layer.mTransmittance *= 1.0 - alpha;

        // **By what it hid and not by what it was lit by.** An unlit puff of smoke sends back no
        // light at all and still decides the whole of what the pixel shows.
        if (alpha > layer.mCovering.mWeight)
            layer.mCovering = SpriteClaim(direction * depth, sprite.mMoved, alpha);
    }

    if (coverage > 0.0)
        layer.mRadiance += covered * ((1.0 - layer.mTransmittance) / coverage);

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
