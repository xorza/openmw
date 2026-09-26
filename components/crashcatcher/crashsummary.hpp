#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "crashnote.hpp"

namespace Crash
{
    /// What the monitor learnt of one report, in words and numbers it can print on any system.
    struct CrashFacts
    {
        NotesRead mNotes;

        /// The exception, as the system names it and with what it says of the fault: "SIGSEGV at
        /// 0x10", "EXCEPTION_ACCESS_VIOLATION reading 0x10". Empty for a report the game asked
        /// for, which faulted nothing.
        std::string mException;

        /// The system's id of the thread that faulted or asked. Nought where none is known.
        std::uint64_t mThread = 0;

        /// The instruction it stopped at, as a module and an offset in it.
        std::string mWhere;

        /// Values on its stack that point into a module's code, nearest first, in the same form.
        /// Candidates and not frames: an exact stack comes from the dump and the release's symbols.
        std::vector<std::string> mStack;

        /// What the game said of itself: the version, the renderer, the device.
        std::vector<std::pair<std::string, std::string>> mAnnotations;

        /// Where the dump is. Empty where none was written.
        std::string mDump;
    };

    /// The summary of `facts`, a line each into `lines`, each without the log's time stamp or a
    /// line end. The same lines on every system, whichever of them wrote the report.
    void summarise(const CrashFacts& facts, std::vector<std::string>& lines);
}
