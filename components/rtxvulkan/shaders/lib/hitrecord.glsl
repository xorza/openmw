#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_HITRECORD_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_HITRECORD_GLSL

// What the launch told the stages a hit invokes, through the record the hit landed on: `HitRecord`
// in `visibility.h` says why it is here and not in the payload. One file, because the any-hit and
// the closest-hit stages share a hit group's record and both read the same fact off it.

#include "camera.h"
#include "visibility.h"

#include "bindings.glsl"

layout(shaderRecordEXT, scalar) buffer HitRecordBlock
{
    HitRecord record;
};

/// The cone the ray this stage answers for was cast with.
///
/// **The record says which eye, and the eye says the cone.** The launch traces the world through
/// `frame.mCamera` and the player's arms through `frame.mArms`, whose spread the host widens to
/// the first-person field of view, and a hit on the arms read at the world eye's narrower pixel
/// was a texel resolved a level too fine. Every other ray in the frame is an inline query inside a
/// shader and never comes through here. A select on a record field, which is uniform per record.
Cone stageCone()
{
    return coneAt(record.mArms != 0u ? frame.mArms : frame.mCamera);
}

#endif
