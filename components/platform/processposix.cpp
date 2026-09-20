#include "process.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
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

    void readCommandOutput(const char* command, std::string& into)
    {
        into.clear();

        const std::string line = std::string(command) + " 2>/dev/null";
        std::FILE* pipe = popen(line.c_str(), "r");
        if (pipe == nullptr)
            return;

        std::array<char, 256> buffer{};
        while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
            into += buffer.data();

        pclose(pipe);
    }
}
