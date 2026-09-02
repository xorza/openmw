// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_REORDER_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_REORDER_GLSL

// What a hit is sorted on, and the one call that sorts on it.
//
// **Ray generation only, and `RTX_REORDERING` is what says so.** `reorderThreadEXT` is defined for
// that stage and no other, and `hitObjectEXT` may only be declared in ray generation, closest-hit and
// miss shaders — while the fog volume, which is a dispatch, reaches this file through
// `reproject.glsl`. So the shader that can reorder defines `RTX_REORDERING` before it includes
// anything, and the call is one piece of source either way.
//
// `.notes/rtx/ser-plan.md` is what these are for and §9 is what they measured.

#include "scene.h"
#include "traversal.glsl"
#include "variants.glsl"

/// Which of the four kinds of shading a ray leads to.
///
/// **The three a surface can be are the material kinds themselves, and so are the hit table's
/// record indices.** That is what makes the sort's first key the kind: a hit object records one of
/// them and the hardware reads the shader it names. Written as the material's own numbers rather
/// than as three more, so a record and a material cannot come to be numbered apart —
/// `VisibilityPass` lists the three closest-hit shaders in this order.
///
/// **The sky is the fourth and is not one of them.** A miss records into the miss table, where
/// `RECORD_SKY` is the only record — so `SORT_SKY` is a sort key and never a record index.
const uint SORT_SURFACE = KIND_SURFACE;
const uint SORT_TERRAIN = KIND_TERRAIN;
const uint SORT_WATER = KIND_WATER;
const uint SORT_SKY = 3u;

const uint RECORD_SKY = 0u;

/// How many bits `reorderKind` needs, for the one form of the call that has no hit object to carry
/// it.
const int REORDER_KIND_BITS = 2;

/// Whether the eye sees through this surface — the one branch in the trace that traverses again.
///
/// **The more significant of the two, because the order matters**: the sources say a more
/// significant hint bit weighs more in the sort key, and a second traversal is a far larger branch
/// than one more texture read.
const uint FLAG_SEEN_THROUGH = 2u;

/// Whether the material carries a mask, which is what makes a hit's own resolve read a texture twice
/// and what makes its traversal stop to ask.
const uint FLAG_MASKED = 1u;

/// How many bits `reorderFlags` is.
///
/// **Two, and the same two on every thread.** A hint bit displaces hit-object information from the
/// sort key, so what is asked for is what a branch below actually turns on and nothing more.
const int REORDER_FLAG_BITS = 2;

/// Which shading is ahead of a hit.
///
/// **One instance row and one material row, which is what `resolve` reads first anyway.** What this
/// answers is the shape of the work — a layer stack, a second traversal into the water, or a plain
/// surface — and not any value that work produces.
uint reorderKind(Hit hit)
{
    if (!hit.mHit)
        return SORT_SKY;

    const GpuMaterial material = materials[instances[hit.mInstance].mMaterial];

    if (material.mKind == KIND_WATER)
        return SORT_WATER;

    // Ground that kept its stack, which is what `resolve` reads four or five masked textures for. A
    // distant chunk was flattened into one texture and is a plain surface from here.
    if (material.mKind == KIND_TERRAIN && material.mDiffuse == NO_TEXTURE)
        return SORT_TERRAIN;

    return SORT_SURFACE;
}

/// What the shader-table index does not already say, as the bits the sort is hinted with.
///
/// **The eye's own under-water state is not here.** It is the same answer for every pixel of a
/// frame, so a bit carrying it would sort nothing and would cost the key two bits of where the hit
/// is.
uint reorderFlags(Hit hit)
{
    if (!hit.mHit)
        return 0u;

    const GpuInstance instance = instances[hit.mInstance];
    const GpuMaterial material = materials[instance.mMaterial];

    const uint through = isSeenThrough(surfaceOpacity(instance, material)) ? FLAG_SEEN_THROUGH : 0u;
    const uint masked = hasMask(material) ? FLAG_MASKED : 0u;

    return through | masked;
}

/// The whole key, for the form of the call that carries no hit object: the kind above the flags.
///
/// **Where there is no hit object there is no shader identifier**, so the kind the identifier would
/// have carried belongs in the hint instead — which is the one case the sources' "do not repeat in
/// the hint what the shader identifier already says" does not cover.
uint reorderKey(Hit hit)
{
    return (reorderKind(hit) << REORDER_FLAG_BITS) | reorderFlags(hit);
}

#ifdef RTX_REORDERING

/// The barycentrics a hit object recorded from a query carries. Nothing reads them back — the `Hit`
/// already holds them — and the extension still wants somewhere to put them.
layout(location = 0) hitObjectAttributeEXT vec2 recordedBarycentrics;

/// Records a hit object out of a committed traversal and reorders on it.
///
/// **A macro for the reason `RTX_TRAVERSE` is one.** `glslc` refuses a `rayQueryEXT` as a parameter
/// and refuses a `hitObjectEXT` as one, so neither the query this records from nor the object it
/// records can cross a function boundary.
///
/// @param query the traversal `RTX_TRAVERSE` left committed, which this reads and does not change.
/// @param hit what that traversal answered, which decides the record and the hint.
/// @param bits how many low bits of `hint` to sort on. **A literal at every call**, so the branch
///        below folds — nought asks for the hit object alone, which is where the sources say to
///        start.
#define RTX_REORDER(query, hit, origin, direction, hint, bits)                                              \
    {                                                                                                       \
        hitObjectEXT sorted;                                                                                \
        if ((hit).mHit)                                                                                     \
            hitObjectRecordFromQueryEXT(sorted, (query), reorderKind(hit), 0);                              \
        else                                                                                                \
            hitObjectRecordMissEXT(                                                                         \
                sorted, gl_RayFlagsNoneEXT, RECORD_SKY, (origin), 0.0, (direction), frame.mFar);            \
                                                                                                            \
        if ((bits) > 0)                                                                                     \
            reorderThreadEXT(sorted, (hint), (bits));                                                       \
        else                                                                                                \
            reorderThreadEXT(sorted);                                                                       \
    }

#else

/// **A block that does nothing, for a stage that cannot reorder.** `reorderThreadEXT` is a ray
/// generation instruction and `hitObjectEXT` may not even be declared outside that stage and the two
/// beside it — and the fog volume, which is a dispatch, reaches this file through `reproject.glsl`
/// for the sprite and water motion it needs. A block rather than nothing at all, so that an `if`
/// whose whole body is one of these does not swallow the statement under it.
#define RTX_REORDER(query, hit, origin, direction, hint, bits)                                              \
    {                                                                                                       \
    }

#endif

#endif
