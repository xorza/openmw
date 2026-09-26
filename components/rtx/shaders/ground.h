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

    /// Where `groundcomposite.comp` binds what it reads and writes in set 0, and how many there
    /// are. The shader's layout and the pass's own layout and writes are numbered by these and by
    /// nothing else, so the two cannot drift apart.
    const uint GROUND_COMPOSITE_BIND_ALBEDO = 0;
    const uint GROUND_COMPOSITE_BIND_GLOSS = 1;
    const uint GROUND_COMPOSITE_BINDINGS = 2;

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

    /// What a bake writes, as bits: the chunk's albedo, its gloss — how much of the ground reflects
    /// in red and its roughness in green, `CompositeQueue` says why a distant chunk needs both — or
    /// both from the one sum, which is what a chunk that arrived with both is baked as.
    const uint GROUND_COMPOSITE_ALBEDO = 1u;
    const uint GROUND_COMPOSITE_GLOSS = 2u;

    /// What one bake is told: the three tables the sum reads, by address as the frame's block
    /// carries them, which chunk, which of its two images, and the array's texel counts, which say
    /// which layer textures stand in.
    struct GroundCompositeConstants
    {
        uint64 mMaterials;
        uint64 mLayers;
        uint64 mMasks;

        /// The chunk's material row, whose layers are summed.
        uint mMaterial;

        /// `GROUND_COMPOSITE_ALBEDO`, `GROUND_COMPOSITE_GLOSS` or both.
        uint mOutputs;

        /// `GpuTables::mTextureTexels`.
        uint64 mTexels;
    };

#ifdef RTX_HOST
    static_assert(
        sizeof(GroundCompositeConstants) == 40, "GroundCompositeConstants must be scalar-packed on every side");
}
#endif

#endif
