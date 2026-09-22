#ifndef OPENMW_COMPONENTS_RTX_SHADERS_SETS_H
#define OPENMW_COMPONENTS_RTX_SHADERS_SETS_H

#include "hosttypes.h"
#include "portable.h"

// Which descriptor set is which, for the shaders that declare them and the layouts and binds that
// place them.
//
// **A set's number was a literal in the shaders and a place in a list on the host**, and three lists
// had to agree with each other and with the literals: the layouts a pipeline was made with, the sets
// a trace bound, and the first set a bind began at. The layers caught a disagreement only because
// the three shared sets happen to hold different descriptor types, and only in a validated run.
// `Rtx::SharedSets` names each by what it holds, and puts it at the number here.
//
// Set zero is each pass's own and is pushed; the others are made once and bound by every pass that
// reads them.

#ifdef RTX_HOST
namespace Rtx::Shaders
{
#endif

    /// The pass's own bindings, pushed with every dispatch — `bindings.h` and each pass's header
    /// number them.
    const uint SET_PASS = 0;

    /// The bindless texture array and its shading maps — `TEXTURE_BIND_*` in `scene.h`.
    const uint SET_TEXTURES = 1;

    /// The channels the trace writes — `CHANNEL_*` in `gbuffer.h`.
    const uint SET_CHANNELS = 2;

    /// The air in front of the camera — `BIND_FOG_*` in `fogvolume.h`.
    const uint SET_VOLUME = 3;

    /// How many sets any pipeline may name.
    const uint SET_COUNT = 4;

#ifdef RTX_HOST
}
#endif

#endif
