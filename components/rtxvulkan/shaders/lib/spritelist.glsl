#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_SPRITELIST_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_SPRITELIST_GLSL

// The sprite tables and the tiles' list over them, as the three bin passes write them and the
// trace reads them.
//
// **One statement of the list's shape, because four shaders indexed it by hand.** The count pass
// and the scan wrote at `1 + tile`, the fill and the trace read at `tile` and `tile + 1`, and the
// rule that makes those one table — entry nought is the head — was prose in `scene.h`. The rect a
// sprite covers was packed in one pass and unpacked in another, sixteen bits a coordinate, with
// the split spelled in both. And each pass declared the tables again with the alignment as a
// literal, where `scene.h` names what each reference may claim.
//
// Nothing here reads the frame, so a pass with no frame block reaches it.

#include "scene.h"

layout(buffer_reference, scalar, buffer_reference_align = TABLE_ALIGN_ROWS) readonly buffer SpriteTable
{
    GpuSprite at[];
};

layout(buffer_reference, scalar, buffer_reference_align = TABLE_ALIGN_ROWS) readonly buffer EmitterTable
{
    GpuEmitter at[];
};

/// What a trace made of each emitter, `GpuEmitterFrame`: written by `spriteemitters.rgen` and read
/// by the walks.
layout(buffer_reference, scalar, buffer_reference_align = TABLE_ALIGN_ROWS) buffer EmitterFrames
{
    GpuEmitterFrame at[];
};

/// The same sprites, for the two passes that write them: the shelter zeroes a drop under a roof
/// and the shade writes each sprite's layers.
layout(buffer_reference, scalar, buffer_reference_align = TABLE_ALIGN_ROWS) buffer WrittenSprites
{
    GpuSprite at[];
};

/// The placement's medium and additive instances, as the spheres they can be met in.
layout(buffer_reference, scalar, buffer_reference_align = TABLE_ALIGN_ROWS) readonly buffer PresenceTable
{
    GpuPresence at[];
};

/// One word of `PRESENCE_` bits a tile, `GpuTables::mSpritePresence`.
layout(buffer_reference, scalar, buffer_reference_align = TABLE_ALIGN_ROWS) buffer SpritePresence
{
    uint at[];
};

/// One packed rect per sprite, which `spriterects.comp` writes and `spriteruns.comp` reads.
layout(buffer_reference, scalar, buffer_reference_align = TABLE_ALIGN_BLOCKS) buffer SpriteRects
{
    uvec2 at[];
};

/// The tiles' list, in `Rtx::RunList`'s shape: entry nought is the head, then where each tile's
/// run starts, then the runs. `SPRITE_LIST_UNBINNED` says what entry nought holds on a frame whose
/// runs did not fit.
layout(buffer_reference, scalar, buffer_reference_align = TABLE_ALIGN_ROWS) buffer SpriteTileList
{
    uint at[];
};

/// Where `tile`'s count accumulates while the sprites are binned. The scan turns that entry in
/// place into where tile `tile + 1`'s run starts, and writes the head into entry nought — which is
/// also where tile nought's run starts, and is what makes the two readings one table.
uint spriteCountSlot(uint tile)
{
    return 1u + tile;
}

/// Where `tile`'s run starts once the scan has run, and `spriteStartSlot(tile + 1)` is where it
/// ends.
uint spriteStartSlot(uint tile)
{
    return tile;
}

/// Which tile a traced pixel is in, on a frame `width` pixels across.
uint spriteTileOf(uvec2 pixel, uint width)
{
    return (pixel.y / SPRITE_TILE) * spriteTilesOver(width) + pixel.x / SPRITE_TILE;
}

/// What the trace leaves at a pixel for the puffs' composite and the curve, `CHANNEL_PUFFS_DEPTH`:
/// how far the puffs stand, what the cloud shells let through, and whether the arms were found there
/// — whose eye the composite marches the pixel's ray from again.
struct PuffsStood
{
    float mCoveredAt;
    float mShellsThrough;
    bool mArms;
};

/// **Two words and a bit, in a channel of two.** The flag rides in the sign of the transmittance,
/// which a transmittance never spends: a float store keeps its sign bit, nought's included, so
/// `abs` gives the transmittance back exactly and the bit gives the flag. A third word is past the
/// format — `GBUFFER_PUFF_DEPTH` is two floats — and a flag stored there reads back as nought.
vec2 packPuffsStood(PuffsStood stood)
{
    return vec2(stood.mCoveredAt, stood.mArms ? -stood.mShellsThrough : stood.mShellsThrough);
}

PuffsStood unpackPuffsStood(vec2 packed)
{
    return PuffsStood(packed.x, abs(packed.y), (floatBitsToUint(packed.y) & 0x80000000u) != 0u);
}

/// The `PRESENCE_` kinds a ray through a traced pixel's tile can meet — every kind where the frame
/// binned nothing, which is a camera that draws no sprites and a frame whose runs did not fit.
///
/// **Uniform over a tile, so a warp takes a walk or leaves it whole.**
uint presenceAt(SpriteTileList list, SpritePresence presence, uint tracedWidth, uvec2 traced)
{
    return list.at[0] == SPRITE_LIST_UNBINNED ? PRESENCE_ADDITIVE | PRESENCE_MEDIUM
                                               : presence.at[spriteTileOf(traced, tracedWidth)];
}

/// Whether the puff layer holds nothing at a traced pixel: no sprite binned into its tile, no cloud
/// shell in front of it, no additive mesh a ray through its tile can meet, and not the arms' — so
/// the composite there would leave the frame as it found it, with a transmittance of one in its
/// alpha. An arms' ray looks its sprites up in another tile than its own (`binnedPixel`), and the
/// hand is a sliver of the frame, so it is walked rather than asked about.
///
/// **Asked by the composite and by the curve, which have to agree pixel for pixel.** The composite
/// skips such a pixel, and the curve reads a transmittance of one there in place of an alpha the
/// composite never wrote. That is most of every frame at the shown extent: on a clear day every
/// pixel paid three loads and a store to write a one, which was 0.46 ms of a 4K frame and 0.27
/// with the writes gone. A frame whose runs did not fit binned nothing, and every pixel of it
/// walks every sprite — `SPRITE_LIST_UNBINNED`.
///
/// @param stood what the trace left at the pixel.
bool puffsCoverNothing(SpriteTileList list, SpritePresence presence, uint tracedWidth, uvec2 traced, PuffsStood stood)
{
    if (list.at[0] == SPRITE_LIST_UNBINNED || stood.mShellsThrough < 1.0 || stood.mArms)
        return false;

    const uint tile = spriteTileOf(traced, tracedWidth);
    return (presence.at[tile] & PRESENCE_ADDITIVE) == 0u
        && list.at[spriteStartSlot(tile)] == list.at[spriteStartSlot(tile + 1u)];
}

/// A tile rect as one `uvec2`: the corner in `x` and the far corner in `y`, sixteen bits a
/// coordinate. Sixteen bits reaches a frame 1,048,576 pixels wide.
uvec2 packSpriteRect(uvec2 from, uvec2 to)
{
    return uvec2(from.x | (from.y << 16u), to.x | (to.y << 16u));
}

void unpackSpriteRect(uvec2 packed, out uvec2 from, out uvec2 to)
{
    from = uvec2(packed.x & 0xFFFFu, packed.x >> 16u);
    to = uvec2(packed.y & 0xFFFFu, packed.y >> 16u);
}

#endif
