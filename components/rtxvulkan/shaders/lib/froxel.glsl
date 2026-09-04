#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_FROXEL_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_FROXEL_GLSL

#include "camera.h"
#include "look.h"
#include "scene.h"
#include "bindings.glsl"
#include "random.glsl"

// Quadratic depth concentrates samples near the eye while keeping fixed boundaries across frames.
float fogDepth(float fraction)
{
    return fraction * fraction;
}

float froxelNear(uint slice)
{
    return fogDepth(float(slice) / float(FOG_VOLUME_SLICES)) * FOG_REACH;
}

float froxelFar(uint slice)
{
    return froxelNear(slice + 1u);
}

// A continuous stride prevents the noise's mip level from stepping at each slice boundary.
float froxelStrideAt(float along)
{
    return 2.0 * sqrt(along * FOG_REACH) / float(FOG_VOLUME_SLICES);
}

// Texel centres lie halfway along the depth coordinate, not halfway between world-space edges.
float froxelMiddle(uint slice)
{
    return fogDepth((float(slice) + 0.5) / float(FOG_VOLUME_SLICES)) * FOG_REACH;
}

float fogVolumeDepth(float along)
{
    return sqrt(min(along, FOG_REACH) / FOG_REACH);
}

// Every fog image shares the allocated grid extent, which may exceed this camera's viewport.
vec2 fogVolumeUV(vec2 pixel, ivec2 extent)
{
    return pixel / float(FOG_VOLUME_SCALE) / vec2(extent);
}

const uint FOG_COLUMN_MASK = MASK_SOLID | MASK_WATER | MASK_FIRST_PERSON;

// rayAt adds half a pixel; inside already measures from the corner of the pixel block.
Ray fogColumnRayAt(uvec2 column, vec2 inside)
{
    return rayAt(frame.mCamera, (vec2(column) + inside) * float(FOG_VOLUME_SCALE) - 0.5);
}

// Depth and scattering must sample the same ray, with a new position inside the block each frame.
Ray fogColumnRay(uvec2 column)
{
    return fogColumnRayAt(
        column, vec2(randomAt(column + uvec2(17u, 5u), STREAM_FOG), randomAt(column + uvec2(3u, 29u), STREAM_FOG)));
}

#endif
