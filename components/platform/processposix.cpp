#include "process.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

#include <unistd.h>

namespace Platform::Process
{
    void trap() noexcept
    {
        __builtin_trap();
    }

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

    std::optional<int> startAgain(char* const argv[], std::string& why)
    {
        // The image itself where the kernel names it, so a tool started through a relative path
        // from a directory the run has since left starts again all the same; the name it was
        // started by where there is no such link.
        execv("/proc/self/exe", argv);
        execvp(argv[0], argv);
        why = std::strerror(errno);
        return std::nullopt;
    }

    std::uint32_t currentId()
    {
        return static_cast<std::uint32_t>(getpid());
    }
}
