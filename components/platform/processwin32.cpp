#include "process.hpp"

#include <cstdint>
#include <cstdlib>

#include <components/misc/windows.hpp>

namespace Platform::Process
{
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
