#include "process.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <intrin.h>

#include <components/misc/windows.hpp>

namespace Platform::Process
{
    void trap() noexcept
    {
        // `int 29h`, which no handler sees, is what MSVC has in place of `ud2`.
        __fastfail(FAST_FAIL_FATAL_APP_EXIT);
    }

    void setEnvironmentDefault(const char* name, const char* value)
    {
        if (std::getenv(name) == nullptr)
            _putenv_s(name, value);
    }

    void readCommandOutput(const char* command, std::string& into)
    {
        into.clear();

        // `cmd` spells the null device `nul`, and would make a file of `/dev/null`.
        const std::string line = std::string(command) + " 2>nul";
        std::FILE* pipe = _popen(line.c_str(), "r");
        if (pipe == nullptr)
            return;

        std::array<char, 256> buffer{};
        while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
            into += buffer.data();

        _pclose(pipe);
    }
}
