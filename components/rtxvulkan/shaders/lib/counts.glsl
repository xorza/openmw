#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_COUNTS_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_COUNTS_GLSL

// Every count a frame takes for the host, `counts.h`, written from here and nowhere else, so the
// block and the constant that gates it are known in one place. The hold's own clock in
// `stress.comp` is not a count and is not here: it binds the block on a layout of its own,
// because it runs on nothing else of the frame's.

#include "bindings.glsl"
#include "variants.glsl"

/// Counts one primary ray that reached nothing, from the miss shader. `FrameCounts::mMisses` says
/// why the misses are counted and the hits derived, and `COUNTING` why the atomic is not in the
/// game's kernel at all.
void countMiss()
{
    if (COUNTING)
        atomicAdd(counts.mMisses, 1u);
}

/// Counts `value` at `boundary` — `BOUNDARY_*` in `counts.h` — where any component of it is a NaN
/// or an infinity. One word for every store that crossed, so a frame carrying one froxel of NaN
/// reads as one and a frame gone black reads as the frame. A NaN is a logic error and is refused
/// nowhere: the count is what turns it into a failed check rather than a picture that goes black
/// by itself.
void countNotFinite(uint boundary, vec4 value)
{
    if (COUNTING && (any(isnan(value)) || any(isinf(value))))
        atomicAdd(counts.mNotFinite[boundary], 1u);
}

/// Stores `value` into `target` at `at`, counted at `boundary` first: the one way a boundary is
/// written, so a channel added beside the others cannot cross it uncounted.
///
/// **A macro because an image with a format is a type of its own in SPIR-V**, and the channels
/// are five formats and two dimensions: a function would take one of them, the way
/// `RTX_TRAVERSE` cannot take a hit object at all. Substituted textually, so `value` is taken
/// once into a local and `target` may name any image.
#define RTX_STORE_COUNTED(target, at, boundary, value)                                                                 \
    {                                                                                                                  \
        const vec4 crossing = (value);                                                                                 \
        countNotFinite((boundary), crossing);                                                                          \
        imageStore((target), (at), crossing);                                                                          \
    }

#endif
