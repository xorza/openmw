#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_RUNS_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_RUNS_GLSL

// A run handed to a dispatch by its own address, indexed from nought: what the pose kernels and
// the probe read, said once.
//
// **Its own file because three passes declared these for themselves**, each with the alignment
// as a literal, where `scene.h` names what a reference may claim. A block is `bindings.glsl`'s
// and is reached through a table of addresses; a run is one address the host hands over whole,
// which `Rtx::SceneDesc` never lets straddle a block.

#include "scene.h"
#include "skinning.h"

// Four, because a twelve-byte element at an arbitrary index is only ever float-aligned.
layout(buffer_reference, buffer_reference_align = TABLE_ALIGN_ROWS, scalar) readonly buffer Vec3Run
{
    vec3 at[];
};

layout(buffer_reference, buffer_reference_align = TABLE_ALIGN_ROWS, scalar) writeonly buffer Vec3Written
{
    vec3 at[];
};

layout(buffer_reference, buffer_reference_align = TABLE_ALIGN_ROWS, scalar) readonly buffer UintRun
{
    uint at[];
};

layout(buffer_reference, buffer_reference_align = TABLE_ALIGN_ROWS, scalar) writeonly buffer UintWritten
{
    uint at[];
};

layout(buffer_reference, buffer_reference_align = TABLE_ALIGN_ROWS, scalar) readonly buffer FloatRun
{
    float at[];
};

layout(buffer_reference, buffer_reference_align = TABLE_ALIGN_ROWS, scalar) readonly buffer InfluenceRun
{
    GpuInfluence at[];
};

layout(buffer_reference, buffer_reference_align = BONE_ALIGN, scalar) readonly buffer BoneRun
{
    GpuBone at[];
};

#endif
