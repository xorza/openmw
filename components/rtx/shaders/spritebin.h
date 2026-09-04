// `#pragma once` everywhere else in this tree, and an include guard here: `glslc` warns
// "'#pragma once' : not implemented" and carries on, so a header included twice by one
// shader would redefine everything in it.
#ifndef OPENMW_COMPONENTS_RTX_SHADERS_SPRITEBIN_H
#define OPENMW_COMPONENTS_RTX_SHADERS_SPRITEBIN_H

#include "camera.h"
#include "portable.h"
#include "scene.h"

// What bins the sprite layer into the screen's tiles on the device, as both sides see it.
//
// **The host used to do this, and a rainstorm is what moved it.** Every sprite was projected twice
// on the processor, its tiles counted and then filled, and the whole list written across the bus —
// half a millisecond of the frame's critical path over Balmora in the rain, and a cost that grew
// with the tile count whatever the sprites did, which is what pinned the tile at sixteen pixels.
// Here the sprites and the emitters are already on the device for the trace to read, so three
// dispatches make the list where it is read: one per sprite for its tiles and their counts, one
// over the tiles for where each run starts, and one per tile that fills its run in order.
//
// **The list comes out in the shape `Rtx::RunList` makes**, because the trace reads that shape
// and the light grid still arrives in it from the host: the first `tiles + 1` entries say where
// each tile's run starts, counted from the front, and the runs follow them. What the host promised
// of it is kept — ascending sprite index within a run, which is the composite order — and it is
// kept by construction rather than by a sort: a tile fills its own run by walking the sprites in
// order, so no atomic ever decides where an entry lands.

#ifdef RTX_HOST

#include <cstdint>

#include <osg/Vec3f>

namespace Rtx::Shaders
{
    using vec3 = osg::Vec3f;
    using uint = std::uint32_t;
    using uint64 = std::uint64_t;

#else

#define uint64 uint64_t

#endif

    /// Lanes that share one sprite in the pass that counts its tiles.
    ///
    /// **A sprite the eye is inside is in every tile**, and one lane counting thousands of tiles
    /// on its own is a spike in a pass that is otherwise a few microseconds. Thirty-two lanes
    /// stride over the rectangle instead, so the worst sprite costs a hundred iterations of a
    /// warp rather than thousands of one thread. A raindrop's one tile leaves the other lanes
    /// idle, and that idleness is cheaper than the spike it prevents.
    const uint SPRITE_BIN_LANES = 32u;

    /// Lanes in one workgroup of the pass over sprites: eight sprites at `SPRITE_BIN_LANES` each.
    const uint SPRITE_BIN_WORKGROUP = 256u;

    /// Lanes in the one workgroup that turns the tile counts into starts.
    ///
    /// **One workgroup and not a multi-pass scan**, because there are at most tens of thousands of
    /// tiles: each lane takes a contiguous chunk and the lanes' totals are scanned in shared
    /// memory, which is a few microseconds at the largest frame this renderer traces.
    const uint SPRITE_SCAN_WORKGROUP = 1024u;

    /// Lanes that share one tile in the pass that fills its run.
    ///
    /// **One lane per tile was measured first, and it is a quarter of a millisecond.** A lane
    /// walking every sprite alone is a chain of a load, a compare and a branch per sprite with
    /// nothing to hide the load's latency behind, and a frame's tiles are only a few thousand
    /// lanes — fifteen workgroups on a card with seventy-six multiprocessors. Thirty-two lanes
    /// take thirty-two sprites at a stride instead, in one coalesced load, and agree on the order
    /// of what matched through a word in shared memory: `spriteruns.comp` says how.
    const uint SPRITE_RUNS_LANES = 32u;

    /// Lanes in one workgroup of the pass over tiles: eight tiles at `SPRITE_RUNS_LANES` each.
    const uint SPRITE_RUNS_WORKGROUP = 256u;

    /// What the three dispatches are handed.
    struct SpriteBinConstants
    {
        uint64 mSprites;
        uint64 mEmitters;

        /// One `uvec2` per sprite: the tile rectangle it reaches, as `x | y << 16` for its first
        /// and its last tile inclusive, or a first past every last where it reaches none. Written
        /// by the pass over sprites and read by the pass over tiles, so a rectangle is worked out
        /// once for its tiles to be counted and once more never.
        uint64 mRects;

        /// The list: `tiles + 1` starts, then the runs. `RunList::getWhole`'s shape.
        uint64 mList;

        /// One `uint` the scan writes: how many entries this frame's runs came to, whether or not
        /// they fit. Host-readable, which is what lets the next frame's buffer be sized to it.
        uint64 mReport;

        /// Where the eye stands. `Camera` carries everything else about the frame, and not this,
        /// for the reason it gives.
        vec3 mOrigin;
        Camera mCamera;

        /// How many sprites there are.
        uint mCount;

        /// How many entries the list has room for after its starts.
        uint mCapacity;
    };

#ifdef RTX_HOST

    static_assert(sizeof(SpriteBinConstants) == 120, "SpriteBinConstants must be scalar-packed on every side");
}

#else

#undef uint64

#endif

#endif
