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

#include <osg/Vec4f>

namespace Rtx::Shaders
{
    using vec4 = osg::Vec4f;
    using uint = std::uint32_t;
    using uint64 = std::uint64_t;

#else

#define uint64 uint64_t

#endif

    /// Threads in the probe's workgroup.
    const uint PROBE_WORKGROUP = 64;

    /// How many ways the probe reads one pattern, and so how many `mCount`-long runs its readings
    /// buffer holds: through a descriptor, through a pointer the host handed over as a push
    /// constant, through a pointer read out of a table and indexed by block, and through a pointer
    /// read out of a uniform block.
    const uint PROBE_READINGS = 4;

    /// What a reference to a row of `ProbeRow`s claims about every address it is constructed from.
    ///
    /// **Sixteen, because that is the largest claim the renderer's own tables make**, and a claim
    /// larger than the truth is undefined behaviour with no message. `GpuLayer` is 48 bytes with two
    /// `vec4` at sixteen and thirty-two, so its reference may claim sixteen and the compiler may
    /// load a `vec4` in one instruction. This is the same shape, read the same way.
    const uint PROBE_ROW_ALIGN = 16;

    struct ProbeConstants
    {
        /// The same buffer bound at set 0 binding 0, by device address.
        uint64 mSource;

        /// How many `vec3`s to read out of it, and how many rows out of `ProbeAddresses::mRows`.
        uint mCount;

        /// Elements per block in the address table at binding 2, which holds the same pattern cut
        /// into separate buffers. This is what `SceneBuffers` would do with `VERTEX_BLOCK`.
        uint mBlock;
    };

    /// The addresses the probe reads out of a uniform block rather than out of a push constant.
    ///
    /// **The construct the frame block carries the scene's tables in**, asked of the device on its
    /// own: a `uint64_t` in a scalar-layout uniform block, converted to a reference and dereferenced.
    struct ProbeAddresses
    {
        /// The same buffer `ProbeConstants::mSource` names.
        uint64 mSource;

        /// `mCount` rows of `ProbeRow`.
        uint64 mRows;
    };

    /// A row the size and shape of `GpuLayer`, read through a reference that claims
    /// `PROBE_ROW_ALIGN`.
    struct ProbeRow
    {
        vec4 mA;
        vec4 mB;
        vec4 mC;
    };

#ifdef RTX_HOST

    static_assert(sizeof(ProbeAddresses) == 16, "ProbeAddresses must be scalar-packed on every side");
    static_assert(sizeof(ProbeRow) == 48, "ProbeRow must be scalar-packed on every side");
}

#else

#undef uint64

#endif

#endif
