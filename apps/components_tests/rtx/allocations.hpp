#pragma once

#include <cstddef>

namespace Rtx::Testing
{
    /// How many times this process has been to the heap for a C++ object.
    ///
    /// Counted by replacing the global `operator new` in the one translation unit that defines this,
    /// which the linker then uses for the whole binary — so every allocation any of this code makes
    /// is here, including the ones inside the standard library.
    ///
    /// **Not `malloc`.** A `--wrap=malloc` would also catch the driver's own allocations, which are
    /// not this renderer's to control and would turn a guard into a source of noise. Everything the
    /// frame path is forbidden to do — construct a `std::string`, grow an unreserved vector, capture
    /// a `std::function`, reach for `make_unique` — arrives here regardless.
    std::size_t getAllocationCount();
}
