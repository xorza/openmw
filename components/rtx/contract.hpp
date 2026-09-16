#pragma once

#include <cstdio>
#include <cstdlib>

namespace Rtx
{
    /// A contract the code keeps, said once for both builds: checked and reported where asserts
    /// are on, and a trap where they are off — one compare and a cold `ud2`, so a lookup the
    /// contract guarantees is not a warning about the path the contract rules out. Not
    /// `__builtin_unreachable`: GCC's `-Wnull-dereference` still names the path an unreachable
    /// rules out, and a violated contract would then run on into whatever it dereferenced. Never
    /// for what the world might supply: a contract is the code's, and untrusted input is a throw.
    inline void contract(const bool held, const char* what)
    {
#ifdef NDEBUG
        if (!held)
            __builtin_trap();
#else
        if (!held)
        {
            std::fputs(what, stderr);
            std::fputc('\n', stderr);
            std::abort();
        }
#endif
    }
}
