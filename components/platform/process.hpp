#pragma once

#include <cstdint>

/// What this process does with itself that the operating systems spell differently: what it says
/// to its environment, and which process it is. One header over
/// `processposix.cpp` and `processwin32.cpp`, the way `file.hpp` sits over its two, and no
/// `#ifdef` anywhere the ray tracer asks these questions.
namespace Platform::Process
{
    /// Gives `name` the value `value` in this process's environment unless the shell already gave
    /// it one: a default the program states, and never a word over the shell's. Answers whether
    /// the default took, so a program whose promise rests on it can say when the shell's word
    /// stood instead.
    bool setEnvironmentDefault(const char* name, const char* value);

    /// Gives `name` the value `value` in this process's environment, over whatever it had.
    void setEnvironment(const char* name, const char* value);

    /// This process's id, as the system numbers processes: what a reading that names processes
    /// tells this one from the rest by.
    std::uint32_t currentId();
}
