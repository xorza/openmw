#include "process.hpp"

#include <cstdint>
#include <cstdlib>

#include <intrin.h>

#include <components/misc/windows.hpp>

namespace Platform::Process
{
    void trap() noexcept
    {
        // `int 29h`, which no handler sees, is what MSVC has in place of `ud2`.
        __fastfail(FAST_FAIL_FATAL_APP_EXIT);
    }

    bool setEnvironmentDefault(const char* name, const char* value)
    {
        if (std::getenv(name) != nullptr)
            return false;

        _putenv_s(name, value);
        return true;
    }

    void setEnvironment(const char* name, const char* value)
    {
        _putenv_s(name, value);
    }

    std::uint32_t currentId()
    {
        return static_cast<std::uint32_t>(GetCurrentProcessId());
    }
}
