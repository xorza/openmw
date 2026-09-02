// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_REORDER_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_REORDER_GLSL

// What a hit is sorted on, and the calls that trace it, sort on it and run the shader it names.
//
// **Ray generation only, and `RTX_REORDERING` is what says so.** The reorder and the execute are
// defined for that stage and no other, and `hitObjectEXT` may only be declared in ray generation,
// closest-hit and miss shaders — while the fog volume, which is a dispatch, reaches this file
// through `reproject.glsl`. So the shader that can reorder defines `RTX_REORDERING` before it
// includes anything, and the call is one piece of source either way.
//
// **The kind is not in the hint, because the record already carries it.** Traversal picks the
// closest-hit shader from the instance's own shader-table offset, which
// `SceneAcceleration::placeRow` writes from its material kind — and the shader a record names is
// the sort key's first component, above the hint and above where the hit is. A hint bit repeating
// it would displace a bit of the last of those for nothing, which is the whitepaper's own rule.
//
// `.notes/rtx/ser-plan.md` is what these are for, §9 is what Stage 1 measured and §10 Stage 2.

#include "scene.h"
#include "traversal.glsl"
#include "variants.glsl"

/// Whether the eye sees through this surface.
///
/// **The more significant of the two, because the order matters** — the whitepaper puts the most
/// significant hint bit highest in the sort key after the shader itself. This is the branch the
/// launch takes right after the call: a see-through hit is peeled and traced again, which is a
/// second traversal and a second execute, and grouping the threads that will do that is the
/// whitepaper's own loop-exit example in this frame's terms.
const uint FLAG_SEEN_THROUGH = 2u;

/// Whether the material carries a mask.
///
/// **What it predicts is a two-sided surface**, which is the branch it turns on after the call: a
/// mesh the content doubled for its back is a sheet only where its material is masked, and a sheet
/// gathers light from the far hemisphere as well as the near one. The mask's own test happened
/// before this, in the any-hit shader, and no reorder reaches back to it.
const uint FLAG_MASKED = 1u;

/// How many bits `reorderFlags` is.
///
/// **Two, and the same two on every thread.** A hint bit displaces hit-object information from the
/// sort key, so what is asked for is what a branch below actually turns on and nothing more.
const int REORDER_FLAG_BITS = 2;

/// What the shader-table record does not already say, as the bits the sort is hinted with.
///
/// **The eye's own under-water state is not here.** It is the same answer for every pixel of a
/// frame, so a bit carrying it would sort nothing and would cost the key two bits of where the hit
/// is.
///
/// @param instanceIndex which row the hit came off, or anything at all where `found` is false.
uint reorderFlags(bool found, uint instanceIndex)
{
    if (!found)
        return 0u;

    const GpuInstance instance = instances[instanceIndex];
    const GpuMaterial material = materials[instance.mMaterial];

    const uint through = isSeenThrough(surfaceOpacity(instance, material)) ? FLAG_SEEN_THROUGH : 0u;
    const uint masked = hasMask(material) ? FLAG_MASKED : 0u;

    return through | masked;
}

#ifdef RTX_REORDERING

/// Traces one ray, sorts the threads on what it found, and runs the shader that names.
///
/// **A macro because a `hitObjectEXT` may not cross a function boundary**, which is the same
/// restriction `RTX_TRAVERSE` is written around for `rayQueryEXT`.
///
/// **The fused calls where the data allows, which is what the sources say to reach for first.**
/// Khronos's guidance is to start with the whole of trace, reorder and execute as one call and to
/// split it only where something has to happen in between. Here that something is the hint: it is
/// read off the instance the traversal landed on, so every mode that carries one has to see the hit
/// object before it can sort. `hit` is the one mode that carries none and is fully fused.
///
/// **Two payloads, because the two phases invoke different shaders.** Traversal reaches the any-hit
/// shader, which tests a cutout and reads nothing; the execute reaches a closest-hit or the miss
/// shader, which fills in the whole of what the frame's tail needs. The whitepaper names this
/// exactly — an any-hit wants a smaller payload than a closest-hit, and giving it the larger one is
/// register pressure paid for fields the invoked shader never touches. The one fused mode cannot
/// have it both ways and takes the shading payload through traversal too.
///
/// **One call reached by every invocation, and the same hint-bit count on each.** A miss is a hit
/// object like any other, so the sky sorts beside the solids rather than skipping the call and
/// leaving its lanes wherever they were.
///
/// @param object filled in with what the ray found, and left readable: the launch takes the ray,
///        the distance and the instance row back off it rather than keeping them live across the
///        sort.
#define RTX_TRACE_AND_SHADE(object, from, tmin, along, mask)                                                \
    {                                                                                                       \
        if (REORDER == REORDER_HIT)                                                                         \
            hitObjectTraceReorderExecuteEXT(object, sceneTop, gl_RayFlagsNoneEXT, (mask), 0u, 0u,           \
                MISS_RECORD_SKY, (from), (tmin), (along), frame.mFar, RTX_PAYLOAD);                         \
        else                                                                                                \
        {                                                                                                   \
            hitObjectTraceRayEXT(object, sceneTop, gl_RayFlagsNoneEXT, (mask), 0u, 0u, MISS_RECORD_SKY,     \
                (from), (tmin), (along), frame.mFar, RTX_TRAVERSAL_PAYLOAD);                                \
                                                                                                            \
            if (REORDER == REORDER_OFF)                                                                     \
                hitObjectExecuteShaderEXT(object, RTX_PAYLOAD);                                             \
            else                                                                                            \
            {                                                                                               \
                const uint sortOn                                                                           \
                    = reorderFlags(hitObjectIsHitEXT(object), hitObjectGetInstanceCustomIndexEXT(object));  \
                                                                                                            \
                if (REORDER == REORDER_HINT)                                                                \
                {                                                                                           \
                    reorderThreadEXT(sortOn, REORDER_FLAG_BITS);                                            \
                    hitObjectExecuteShaderEXT(object, RTX_PAYLOAD);                                         \
                }                                                                                           \
                else                                                                                        \
                    hitObjectReorderExecuteEXT(object, sortOn, REORDER_FLAG_BITS, RTX_PAYLOAD);             \
            }                                                                                               \
        }                                                                                                   \
    }

#endif

#endif
