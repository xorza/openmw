#include "process.hpp"

#include <cstdint>
#include <cstdlib>

#include <unistd.h>

namespace Platform::Process
{
    bool setEnvironmentDefault(const char* name, const char* value)
    {
        if (std::getenv(name) != nullptr)
            return false;

        setenv(name, value, 0);
        return true;
    }

    void setEnvironment(const char* name, const char* value)
    {
        setenv(name, value, 1);
    }

    std::uint32_t currentId()
    {
        return static_cast<std::uint32_t>(getpid());
    }
}
