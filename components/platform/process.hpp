#ifndef OPENMW_COMPONENTS_PLATFORM_PROCESS_HPP
#define OPENMW_COMPONENTS_PLATFORM_PROCESS_HPP

#include <cstdint>
#include <optional>
#include <string>

/// What this process does with itself that the operating systems spell differently: how it
/// ends, what it says to its environment, and which process it is. One header over
/// `processposix.cpp` and `processwin32.cpp`, the way `file.hpp` sits over its two, and no
/// `#ifdef` anywhere the ray tracer asks these questions.
namespace Platform::Process
{
    /// Ends the process at once, where a build without asserts found a contract broken: one
    /// instruction no handler sees, so the compiler treats the path after it as ruled out.
    [[noreturn]] void trap() noexcept;

    /// Gives `name` the value `value` in this process's environment unless the shell already gave
    /// it one: a default the program states, and never a word over the shell's. Answers whether
    /// the default took, so a program whose promise rests on it can say when the shell's word
    /// stood instead.
    bool setEnvironmentDefault(const char* name, const char* value);

    /// Gives `name` the value `value` in this process's environment, over whatever it had: a
    /// word this process leaves for the one it starts in its place with `startAgain`.
    void setEnvironment(const char* name, const char* value);

    /// Starts this process again: the same image, handed `argv` again and the environment as it
    /// stands now. Where the platform can put the fresh process in this one's place it never
    /// returns; where it cannot, it runs the fresh one to its end in this console and answers its
    /// exit status, for the caller to end with. Nothing where neither happened, with why in
    /// `why`; the caller then says so and ends.
    std::optional<int> startAgain(char* const argv[], std::string& why);

    /// This process's id, as the system numbers processes: what a reading that names processes
    /// tells this one from the rest by.
    std::uint32_t currentId();
}

#endif // OPENMW_COMPONENTS_PLATFORM_PROCESS_HPP
