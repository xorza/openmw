#include "process.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace Platform::Process
{
    void trap() noexcept
    {
        __builtin_trap();
    }

    void setEnvironmentDefault(const char* name, const char* value)
    {
        setenv(name, value, 0);
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
