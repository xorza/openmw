#ifndef OPENMW_COMPONENTS_RTX_SHADERS_MIPCHAIN_H
#define OPENMW_COMPONENTS_RTX_SHADERS_MIPCHAIN_H

#include "hosttypes.h"
#include "portable.h"

// The chain a file did not carry, made on the device: what `mipchain.comp` is told about the level
// it writes. `Rtx::MipChain` says what the chain is and why.

#ifdef RTX_HOST
namespace Rtx::Shaders
{
#endif

    /// Where `mipchain.comp` binds what it reads and writes in set 0, and how many there are. The
    /// shader's layout and the pass's own layout and writes are numbered by these and by nothing
    /// else, so the two cannot drift apart.
    const uint MIPCHAIN_BIND_SOURCE = 0;
    const uint MIPCHAIN_BIND_ABOVE = 1;
    const uint MIPCHAIN_BIND_INTO = 2;
    const uint MIPCHAIN_BINDINGS = 3;

    /// The chain's workgroup, square.
    const uint MIP_CHAIN_WORKGROUP = 16u;

    /// One level of the chain, which is one dispatch.
    struct MipChainConstants
    {
        /// Which level is written: the finest is the file's own, fetched from the upload, and
        /// every other is the box over the one above it.
        uint mLevel;

        uint mWidth;
        uint mHeight;

        /// Whether the bytes are display-encoded, so the box averages in light and writes back
        /// encoded; nought for a format with no curve under it.
        uint mEncoded;
    };

#ifdef RTX_HOST
    static_assert(sizeof(MipChainConstants) == 16, "MipChainConstants must be scalar-packed on every side");
}
#endif

#endif
