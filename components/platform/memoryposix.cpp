#include "memory.hpp"

#include <cstdlib>

namespace Platform::Memory
{
    void* allocateAligned(const std::size_t size, const std::size_t alignment) noexcept
    {
        // `aligned_alloc` is specified only for a size that is a multiple of the alignment, so the
        // request is rounded up to one.
        const std::size_t rounded = ((size == 0 ? 1 : size) + alignment - 1) / alignment * alignment;
        return std::aligned_alloc(alignment, rounded);
    }

    void freeAligned(void* memory) noexcept
    {
        std::free(memory);
    }
}
