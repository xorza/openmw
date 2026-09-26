#pragma once

#include <components/crashcatcher/crash.hpp>

namespace Rtx
{
    /// A contract the code keeps, said once for every build: one compare and a cold call that never
    /// returns, which ends the process as a crash whose report names `what`. Not a trap, which the
    /// builds players run would report as `SIGILL` and nothing else, and not
    /// `__builtin_unreachable`: GCC's `-Wnull-dereference` still names the path an unreachable
    /// rules out, and a violated contract would then run on into whatever it dereferenced. A path
    /// no contract compare guards, such as the end of a switch that names every case, calls
    /// `Crash::fatal` itself. Never for what the world might supply: a contract is the code's, and
    /// untrusted input is a throw.
    inline void contract(const bool held, const char* what)
    {
        if (!held)
            Crash::fatal(what);
    }
}
