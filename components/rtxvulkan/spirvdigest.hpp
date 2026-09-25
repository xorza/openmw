#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace Rtx
{
    /// A digest of the program a module holds, read off `spirv-dis --raw-id --no-header`: two
    /// modules digest alike when they hold the same instructions, whatever ids they number them by
    /// and in whatever order they declare their types, constants and variables.
    ///
    /// **Its own canonical form, because the one `spirv-opt --canonicalize-ids` gives is not one.**
    /// That pass is a port of `spirv-remap`, made for compression: it hashes a type from part of its
    /// operands — not a float's width, not a pointer's storage class — into a few thousand ids, and
    /// settles a collision by handing the next free id to whichever type the module declared first.
    /// Moving the specular table's lookup into four functions left a vanilla kernel the same
    /// instruction for instruction and renumbered some of its types.
    ///
    /// **A global is named by what it is**: its opcode, its literals and its decorations, refined
    /// round by round by what its operands are named, until a round tells no two apart that the
    /// round before did not — colour refinement, which reads no id and no order, and which the
    /// pointer cycles `OpTypeForwardPointer` makes do not stop. **An id a function defines is named
    /// by where it is defined**, since the order of a function's instructions is its program. What
    /// has no order — the capabilities, the entry point's interface, the execution modes, the
    /// globals — is sorted. The digest is `MurmurHash3_x64_128`'s, as `digestShaders`' is.
    ///
    /// **Instructions and not values**: two programs that compute one value by different
    /// instructions digest apart. Debug instructions are skipped, since they are not the program.
    ///
    /// Throws `std::runtime_error` for a line it cannot read and for an id nothing defines, naming it,
    /// so a digest is never of a program read in part.
    std::array<std::uint64_t, 2> digestProgram(std::string_view disassembly);
}
