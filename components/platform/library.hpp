#ifndef OPENMW_COMPONENTS_PLATFORM_LIBRARY_HPP
#define OPENMW_COMPONENTS_PLATFORM_LIBRARY_HPP

#include <cstdint>

/// A shared library opened by name at run time and asked for its symbols — the driver's
/// management library is the one the ray tracer opens, because a build must not link what only
/// one vendor's driver ships. One header over `libraryposix.cpp` and `librarywin32.cpp`, the way
/// `file.hpp` sits over its two.
namespace Platform::Library
{
    enum class Handle : std::intptr_t
    {
        Invalid = 0
    };

    /// Opens the library the system finds under `name`, by its own search — a bare file name and
    /// never a path, so the driver's copy is the one found. Invalid where there is none.
    Handle open(const char* name);

    /// The address `symbol` has in `handle`, or null where it exports none by that name.
    void* find(Handle handle, const char* symbol);

    void close(Handle handle);

    class ScopedHandle
    {
        Handle mHandle{ Handle::Invalid };

    public:
        ScopedHandle() noexcept = default;
        ScopedHandle(const ScopedHandle& other) = delete;
        explicit ScopedHandle(Handle handle) noexcept
            : mHandle(handle)
        {
        }
        ScopedHandle(ScopedHandle&& other) noexcept
            : mHandle(other.mHandle)
        {
            other.mHandle = Handle::Invalid;
        }
        ScopedHandle& operator=(const ScopedHandle& other) = delete;
        ScopedHandle& operator=(ScopedHandle&& other) noexcept
        {
            if (mHandle != Handle::Invalid)
                close(mHandle);
            mHandle = other.mHandle;
            other.mHandle = Handle::Invalid;
            return *this;
        }
        ~ScopedHandle()
        {
            if (mHandle != Handle::Invalid)
                close(mHandle);
        }

        Handle get() const noexcept { return mHandle; }
        bool isOpen() const noexcept { return mHandle != Handle::Invalid; }
    };
}

#endif // OPENMW_COMPONENTS_PLATFORM_LIBRARY_HPP
