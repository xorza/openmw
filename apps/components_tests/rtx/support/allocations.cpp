#include "allocations.hpp"

#include <atomic>
#include <cstdlib>
#include <new>

#include <components/platform/memory.hpp>

namespace
{
    // Relaxed because nothing orders anything by it: it is read once, after the work that moved it
    // has been waited on.
    std::atomic<std::size_t> sAllocations{ 0 };

    void* allocate(std::size_t size, std::align_val_t alignment)
    {
        sAllocations.fetch_add(1, std::memory_order_relaxed);
        return Platform::Memory::allocateAligned(size, static_cast<std::size_t>(alignment));
    }

    void* allocate(std::size_t size)
    {
        sAllocations.fetch_add(1, std::memory_order_relaxed);
        return std::malloc(size == 0 ? 1 : size);
    }
}

namespace Rtx::Testing
{
    std::size_t getAllocationCount()
    {
        return sAllocations.load(std::memory_order_relaxed);
    }
}

// Every form the standard names, because the compiler pairs them: a `new` that reached a replaced
// operator and a `delete` that reached the library's own would be freeing with the wrong allocator.
// The aligned forms free through the platform, whose runtime may pair a freer of its own with them.

void* operator new(std::size_t size)
{
    if (void* const memory = allocate(size))
        return memory;

    throw std::bad_alloc();
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    return allocate(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    return allocate(size);
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
    if (void* const memory = allocate(size, alignment))
        return memory;

    throw std::bad_alloc();
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return ::operator new(size, alignment);
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return allocate(size, alignment);
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return allocate(size, alignment);
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::align_val_t) noexcept
{
    Platform::Memory::freeAligned(memory);
}

void operator delete[](void* memory, std::align_val_t) noexcept
{
    Platform::Memory::freeAligned(memory);
}

void operator delete(void* memory, std::size_t, std::align_val_t) noexcept
{
    Platform::Memory::freeAligned(memory);
}

void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept
{
    Platform::Memory::freeAligned(memory);
}

void operator delete(void* memory, const std::nothrow_t&) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, const std::nothrow_t&) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::align_val_t, const std::nothrow_t&) noexcept
{
    Platform::Memory::freeAligned(memory);
}

void operator delete[](void* memory, std::align_val_t, const std::nothrow_t&) noexcept
{
    Platform::Memory::freeAligned(memory);
}
