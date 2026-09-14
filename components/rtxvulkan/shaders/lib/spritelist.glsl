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
