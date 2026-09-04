// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTX_SHADERS_BINDINGS_H
#define OPENMW_COMPONENTS_RTX_SHADERS_BINDINGS_H

#include "portable.h"

// Where set 0's inputs are bound, for the shader that declares them and the pass that writes them.
//
// **Two lists of the same numbers, kept in step by hand, is how a shader comes to read a table
// nobody wrote.** The pass builds its layout and its writes by index and the shader declares each
// binding by number; nothing but a count checked at the end connected the two, so a binding added
// in one place and forgotten in the other was a descriptor left unwritten and a dispatch reading
// whatever the slot held.
//
// **The scene's tables are not here.** They travel as addresses in the frame block — `GpuTables` in
// `scene.h` — so what is left to bind is what has no table to ride in: the structure, the hit
// counter, the block itself, and the images the trace samples.
//
// Set 0 alone. The other three are a bindless texture array, the channels the trace writes and the
// air in front of the camera, and each of those is one owner's to number.
//
// **Here rather than beside the shader that declares them**, because this is where a header both
// languages read has to sit: `portable.h` is next to it, and the shader compiler is given this
// directory and no other.

#ifdef RTX_HOST

#include <cstdint>

namespace Rtx::Shaders
{
    using uint = std::uint32_t;

#endif

    /// The top-level structure every ray is traced against.
    const uint BIND_SCENE = 0;

    /// The atomic a specialized trace counts its hits into.
    const uint BIND_HITS = 1;

    /// The one uniform: everything the frame itself says, and where every table is.
    const uint BIND_FRAME = 2;

    /// The sea's cascades and the fog's field, which are sampled rather than read.
    const uint BIND_WAVE_SURFACE = 3;
    const uint BIND_WAVE_CURVATURE = 4;
    const uint BIND_FOG_FIELD = 5;

    /// How many the set declares, which is the last of them and one more.
    const uint BIND_COUNT = 6;

#ifdef RTX_HOST
}
#endif

#endif
