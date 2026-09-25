#ifndef OPENMW_COMPONENTS_RTX_SHADERS_PINNING_H
#define OPENMW_COMPONENTS_RTX_SHADERS_PINNING_H

#include "hosttypes.h"
#include "portable.h"

// What the pinning probe is handed and what it writes back. Included verbatim by both sides, for the
// reason `visibility.h` is.
//
// **The probe is the build's pinning put to the device.** `pinning.comp` computes each operation
// `Rtx::pinFloatArithmetic` rewrites, the host computes the order the rewrite states with one
// rounding per step and `std::fma` for each fusion, and the two have to agree to the bit — which
// they do only where the device rounds `OpFmaKHR` once and keeps every `NoContraction` step apart.
//
// **An operation with a core the device computes is checked around that core.** `normalize` has an
// inverse square root in it, `smoothstep` and `mod` a division, `refract` a square root, and no host
// computes those to the device's bit. So the probe writes the core as well, computed from the same
// pinned steps, and the host builds everything around it from the device's value: what is checked is
// every step the pinning fixed, and nothing the device was left.

#ifdef RTX_HOST
namespace Rtx::Shaders
{
#endif

    /// Threads in the probe's workgroup.
    const uint PINNING_WORKGROUP = 64;

    /// The floats the probe writes for each case, in the order `pinning.comp` lists them.
    const uint PINNING_RESULTS = 30;

    /// Three vectors, whose `xyz` are the operands and whose `w` are a fourth component, a blend and
    /// a vector the matrix is applied to.
    struct PinningCase
    {
        vec4 mA;
        vec4 mB;
        vec4 mC;
    };

    struct PinningConstants
    {
        uint mCount;
    };

#ifdef RTX_HOST
    static_assert(sizeof(PinningCase) == 48, "PinningCase must be scalar-packed on every side");
}
#endif

#endif
