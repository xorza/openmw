#ifndef OPENMW_COMPONENTS_PLATFORM_PROCESS_HPP
#define OPENMW_COMPONENTS_PLATFORM_PROCESS_HPP

#include <string>

/// What this process does with itself that the operating systems spell differently: how it
/// ends, what it says to its environment, and what it hears from a child. One header over
/// `processposix.cpp` and `processwin32.cpp`, the way `file.hpp` sits over its two, and no
/// `#ifdef` anywhere the ray tracer asks these questions.
namespace Platform::Process
{
    /// Ends the process at once, where a build without asserts found a contract broken: one
    /// instruction no handler sees, so the compiler treats the path after it as ruled out.
    [[noreturn]] void trap() noexcept;

    /// Gives `name` the value `value` in this process's environment unless the shell already gave
    /// it one: a default the program states, and never a word over the shell's.
    void setEnvironmentDefault(const char* name, const char* value);

    /// What `command` wrote to standard output when run through the platform's shell, with its
    /// standard error discarded. A tool that is not there, or that refuses the question, leaves
    /// `into` empty rather than printing a shell's complaint into the middle of a report.
    ///
    /// **The exit status is not read, only what was written.** The reading itself is the test, and a
    /// status this cannot always get — a host that reaps its own children answers -1 — must not
    /// throw a good reading away.
    void readCommandOutput(const char* command, std::string& into);
}

#endif // OPENMW_COMPONENTS_PLATFORM_PROCESS_HPP
