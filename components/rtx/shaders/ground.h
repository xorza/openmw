#ifndef OPENMW_COMPONENTS_RTX_SHADERS_GROUND_H
#define OPENMW_COMPONENTS_RTX_SHADERS_GROUND_H

#include "hosttypes.h"
#include "portable.h"

// A chunk of ground flattened into one texture — the shading LOD: a distant chunk carries every
// ground type in many cells, and distant hits are most of the pixels. `groundcomposite.comp` makes
// one on the device from the chunk's own stack, in the placement the chunk's material row is
// written in, and what it is told is here.

#ifdef RTX_HOST
namespace Rtx::Shaders
{
#endif

    /// How large a composite is, square: the rasterizer's `composite map resolution` default,
    /// stated here because this path forces that setting past every chunk. A power of two, so the
    /// chain halves exactly and ends at one texel.
    const uint GROUND_COMPOSITE_EXTENT = 512u;

    /// How much painted light a bake divides out: full, because a composite cannot be corrected
    /// later — the estimate repeats with a texture's tiling and a composite has none — so
    /// `--delight` reaches the near field and not distant ground.
    const float GROUND_COMPOSITE_DELIGHT = 1.0f;

    /// The bake's workgroup, square.
    const uint GROUND_COMPOSITE_WORKGROUP = 16u;

    /// What one bake is told: the three tables the sum reads, by address as the frame's block
    /// carries them, and which chunk.
    struct GroundCompositeConstants
    {
        uint64 mMaterials;
        uint64 mLayers;
        uint64 mMasks;

        /// The chunk's material row, whose layers are summed.
        uint mMaterial;
    };

#ifdef RTX_HOST
    static_assert(
        sizeof(GroundCompositeConstants) == 32, "GroundCompositeConstants must be scalar-packed on every side");
}
#endif

#endif
