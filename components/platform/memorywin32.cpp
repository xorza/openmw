#include "memory.hpp"

#include <malloc.h>

namespace Platform::Memory
{
    void* allocateAligned(const std::size_t size, const std::size_t alignment) noexcept
    {
        return _aligned_malloc(size == 0 ? 1 : size, alignment);
    }

    void freeAligned(void* memory) noexcept
    {
        _aligned_free(memory);
    }
}
