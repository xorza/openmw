#ifndef OPENMW_COMPONENTS_PLATFORM_MEMORY_HPP
#define OPENMW_COMPONENTS_PLATFORM_MEMORY_HPP

#include <cstddef>

/// The one allocation the C runtimes name differently. One header over `memoryposix.cpp` and
/// `memorywin32.cpp`, the way `file.hpp` sits over its two.
namespace Platform::Memory
{
    /// `size` bytes at `alignment`, or null. Zero bytes is still a distinct pointer, as `new`
    /// promises. Freed by `freeAligned` and nothing else: the runtime that has `aligned_alloc` frees
    /// it with `free`, and the one that does not pairs its own two.
    void* allocateAligned(std::size_t size, std::size_t alignment) noexcept;
    void freeAligned(void* memory) noexcept;
}

#endif // OPENMW_COMPONENTS_PLATFORM_MEMORY_HPP
