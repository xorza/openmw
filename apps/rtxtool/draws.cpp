// The C library's random draws, with the driver's kept apart from the world's.
//
// **`osgParticle` draws every particle from `std::rand`, and so does the driver.** Every range a
// particle reads goes through `rand`, so where the rain falls is a function of how many draws came
// before — and `libnvidia-glcore` draws from the same generator, twice, on the thread that called
// it, at a moment that is the driver's: measured over twenty seconds of `balmora-mages-guild`, at
// frame 639 of one run and 842 of another, on the main thread with no other thread drawing. Two
// draws further along, every drop of the storm that followed fell somewhere else for the rest of
// the run, with the table the scene handed the frame differing on every frame and nothing else in
// the world moved. A draw the driver makes is answered from a generator of its own here, so the
// world's sequence is the world's.
//
// **Defined in the executable and not in a library**, because that is where the dynamic linker
// binds a shared library's call: `osgParticle` and the driver call `rand` through their tables,
// and an executable that exports the symbol is what those tables resolve to — `LD_DEBUG=bindings`
// shows `libnvidia-glcore` bound here. The build exports the two names itself, so the binding does
// not hang on some other library happening to reference them. The world's draws go to `random`,
// which is what the C library's `rand` is on this platform, so a run that never reaches this file
// draws the same numbers as one that does.
//
// **Who is calling is read off the return address**, which is the caller's own code: a module of
// the driver's, or not. The modules are listed from the loader and searched by address; the list
// is taken once per module the process loads, which is the first draw from inside one.
//
// **Linux, because the mechanism is.** The other platform's C library keeps a generator per
// thread and its driver is another driver; a run there answers the question over again.
#ifdef __linux__

#include <link.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <vector>

namespace
{
    /// One loaded segment's addresses, and whether it is the driver's.
    struct Module
    {
        std::uintptr_t mFrom = 0;
        std::uintptr_t mTo = 0;
        bool mDriver = false;
    };

    // Constant-initialised, every one, because `rand` can run before this file's own
    // initialisers would: a library's constructor draws as the library is loaded.
    constinit std::mutex sMutex;

    /// Sorted by where each begins, so a caller is found by one search.
    constinit std::vector<Module> sModules;

    /// The driver's own generator: `minstd`'s state, stepped by hand so that it needs no
    /// constructor. What it draws is its own business, and its sequence need not repeat.
    constinit std::uint32_t sDriverState = 1;

    bool isDriver(const char* const name)
    {
        return name != nullptr && (std::strstr(name, "nvidia") != nullptr || std::strstr(name, "cuda") != nullptr);
    }

    int addModule(dl_phdr_info* const info, std::size_t, void* const into)
    {
        auto& modules = *static_cast<std::vector<Module>*>(into);
        const bool driver = isDriver(info->dlpi_name);
        for (int at = 0; at < info->dlpi_phnum; ++at)
        {
            const ElfW(Phdr)& header = info->dlpi_phdr[at];
            if (header.p_type != PT_LOAD)
                continue;

            const std::uintptr_t from = info->dlpi_addr + header.p_vaddr;
            modules.push_back(Module{ .mFrom = from, .mTo = from + header.p_memsz, .mDriver = driver });
        }

        return 0;
    }

    std::vector<Module> listModules()
    {
        std::vector<Module> modules;
        dl_iterate_phdr(addModule, &modules);
        std::sort(modules.begin(), modules.end(),
            [](const Module& left, const Module& right) { return left.mFrom < right.mFrom; });

        return modules;
    }

    const Module* find(const std::vector<Module>& modules, const std::uintptr_t address)
    {
        const auto after = std::upper_bound(modules.begin(), modules.end(), address,
            [](const std::uintptr_t at, const Module& module) { return at < module.mFrom; });
        if (after == modules.begin())
            return nullptr;

        const Module& module = *std::prev(after);
        return address < module.mTo ? &module : nullptr;
    }

    /// Whether `caller` is inside a module of the driver's.
    bool fromDriver(const void* const caller)
    {
        const auto address = reinterpret_cast<std::uintptr_t>(caller);
        {
            const std::lock_guard<std::mutex> lock(sMutex);
            if (const Module* const module = find(sModules, address); module != nullptr)
                return module->mDriver;
        }

        // Listed with the lock let go, because the loader holds a lock of its own while it runs a
        // library's constructors, and a constructor may draw: a list taken under this lock would
        // wait on the loader while a constructor's draw waited on this.
        std::vector<Module> modules = listModules();
        const Module* const module = find(modules, address);
        const bool driver = module != nullptr && module->mDriver;

        const std::lock_guard<std::mutex> lock(sMutex);
        sModules.swap(modules);
        return driver;
    }

    int driverDraw()
    {
        const std::lock_guard<std::mutex> lock(sMutex);
        sDriverState = static_cast<std::uint32_t>(static_cast<std::uint64_t>(sDriverState) * 48271u % 2147483647u);
        return static_cast<int>(sDriverState);
    }
}

extern "C" int rand() noexcept
{
    if (fromDriver(__builtin_return_address(0)))
        return driverDraw();

    return static_cast<int>(random());
}

extern "C" void srand(const unsigned seed) noexcept
{
    srandom(seed);
}

#endif
