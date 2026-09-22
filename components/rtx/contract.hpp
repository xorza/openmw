#pragma once

#include <cstdio>
#include <cstdlib>

#include <components/platform/process.hpp>

namespace Rtx
{
    /// A path a contract rules out, reached: reported and ended where asserts are on, and a trap
    /// where they are off. For the end of a switch that names every case, and for the call only a
    /// broken caller makes.
    [[noreturn]] inline void broken([[maybe_unused]] const char* what)
    {
#ifdef NDEBUG
        Platform::Process::trap();
#else
        std::fputs(what, stderr);
        std::fputc('\n', stderr);
        std::abort();
#endif
    }

    /// A contract the code keeps, said once for both builds: checked and reported where asserts
    /// are on, and a trap where they are off — one compare and a cold call that never returns, so
    /// a lookup the contract guarantees is not a warning about the path the contract rules out. Not
    /// `__builtin_unreachable`: GCC's `-Wnull-dereference` still names the path an unreachable
    /// rules out, and a violated contract would then run on into whatever it dereferenced. Never
    /// for what the world might supply: a contract is the code's, and untrusted input is a throw.
    inline void contract(const bool held, const char* what)
    {
        if (!held)
            broken(what);
    }
}
