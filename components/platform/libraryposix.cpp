#include "library.hpp"

#include <dlfcn.h>

namespace Platform::Library
{
    Handle open(const char* name)
    {
        // `RTLD_LOCAL`, so the library's names stay its own: nothing here resolves against it but
        // the `find` calls that name what they want.
        return static_cast<Handle>(reinterpret_cast<std::intptr_t>(dlopen(name, RTLD_NOW | RTLD_LOCAL)));
    }

    void* find(const Handle handle, const char* symbol)
    {
        return dlsym(reinterpret_cast<void*>(static_cast<std::intptr_t>(handle)), symbol);
    }

    void close(const Handle handle)
    {
        dlclose(reinterpret_cast<void*>(static_cast<std::intptr_t>(handle)));
    }
}
