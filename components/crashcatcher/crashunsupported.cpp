#include "crash.hpp"

#include <cstdio>
#include <cstdlib>

// **No crash catcher where Crashpad does not run**, FreeBSD among them: `install` says so, and the
// log carries it. Notes are still taken, because `crashnote.cpp` is the same everywhere.
namespace Crash
{
    void runMonitorIfAsked(int, char**) {}

    std::optional<std::string> install(const Settings&)
    {
        return "Crashpad does not support this system";
    }

    void setLogFile(const std::filesystem::path&) {}

    void setHangLimit(std::chrono::seconds) {}

    void heartbeat() {}

    void annotate(std::string_view, std::string_view) {}

    void report(std::string_view) {}

    void fatal(std::string_view reason)
    {
        std::fprintf(stderr, "Fatal: %.*s\n", static_cast<int>(reason.size()), reason.data());
        std::abort();
    }
}
