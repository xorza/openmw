#include "library.hpp"

#include <windows.h>

namespace Platform::Library
{
    Handle open(const char* name)
    {
        // The system's own directories and nothing relative to the process, which is where a
        // driver's library is and where a stray copy beside the binary is not.
        return static_cast<Handle>(
            reinterpret_cast<std::intptr_t>(LoadLibraryExA(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)));
    }

    void* find(const Handle handle, const char* symbol)
    {
        return reinterpret_cast<void*>(
            GetProcAddress(reinterpret_cast<HMODULE>(static_cast<std::intptr_t>(handle)), symbol));
    }

    void close(const Handle handle)
    {
        FreeLibrary(reinterpret_cast<HMODULE>(static_cast<std::intptr_t>(handle)));
    }
}
