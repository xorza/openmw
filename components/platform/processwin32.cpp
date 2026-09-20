#include "process.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <optional>
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

    std::optional<int> startAgain(char* const[], std::string& why)
    {
        // **The command line as the shell gave it, and not `argv` joined back together**: `_execv`
        // joins the arguments with spaces and quotes none of them, so a path with a space in it
        // starts a different run. Nothing on Windows replaces an image, so the fresh process runs
        // in this console to its end, with the handles and the environment this one has — the
        // word `setEnvironment` left included — and its status is this one's.
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION started{};

        // A copy, because `CreateProcessW` may write into the line it is given.
        std::wstring line(GetCommandLineW());
        if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &started))
        {
            why = "CreateProcess failed with error " + std::to_string(GetLastError());
            return std::nullopt;
        }

        CloseHandle(started.hThread);
        WaitForSingleObject(started.hProcess, INFINITE);

        DWORD status = 1;
        if (!GetExitCodeProcess(started.hProcess, &status))
            status = 1;
        CloseHandle(started.hProcess);

        return static_cast<int>(status);
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
