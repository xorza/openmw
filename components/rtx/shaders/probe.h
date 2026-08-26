// `#pragma once` everywhere else in this tree, and an include guard here: `glslc` warns
// "'#pragma once' : not implemented" and carries on, so a header included twice by one
// shader would redefine everything in it.
#ifndef OPENMW_COMPONENTS_RTX_SHADERS_PROBE_H
#define OPENMW_COMPONENTS_RTX_SHADERS_PROBE_H

#include "portable.h"

// What a device-behaviour probe is handed. Included verbatim by both sides, for the reason
// `visibility.h` is.
//
// **Nothing the renderer draws goes through this.** It exists so that an assumption about what the
// hardware does with a construct — a pointer read, a layout, a descriptor left naming something
// destroyed — can be stated as a test that fails, instead of being put to a whole traced frame and
// answered by elimination.

#ifdef RTX_HOST

#include <cstdint>

namespace Rtx::Shaders
{
    using uint = std::uint32_t;
    using uint64 = std::uint64_t;

#else

#define uint64 uint64_t

#endif

    /// Threads in the probe's workgroup.
    RTX_CONST uint PROBE_WORKGROUP = 64;

    /// How many ways the probe reads one pattern, and so how many `mCount`-long runs its readings
    /// buffer holds: through a descriptor, through a pointer the host handed over, and through a
    /// pointer read out of a table and indexed by block.
    RTX_CONST uint PROBE_READINGS = 3;

    struct ProbeConstants
    {
        /// The same buffer bound at set 0 binding 0, by device address.
        uint64 mSource;

        /// How many `vec3`s to read out of it.
        uint mCount;

        /// Elements per block in the address table at binding 2, which holds the same pattern cut
        /// into separate buffers. This is what `SceneBuffers` would do with `VERTEX_BLOCK`.
        uint mBlock;
    };

#ifdef RTX_HOST
}
#else
#undef uint64
#endif

#endif
