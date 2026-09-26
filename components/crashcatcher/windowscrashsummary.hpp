#pragma once

#include <components/misc/windows.hpp>

namespace Crash
{
    /// Appends a crash's summary to the log at `logFile`, which the process holds open: the
    /// exception, every frame the faulting thread unwinds to as a module and an offset in it, and
    /// every thread's last `note`, the calling thread's first.
    ///
    /// **As a crash handler must**, on the thread that faulted: the text is built on the stack
    /// and written straight to the file, with no heap and no lock but the kernel's — the log's own
    /// stream holds a mutex, and the crash may have been inside it. What does not fit is cut. A
    /// release's program database resolves each offset to a line.
    void appendCrashSummary(const wchar_t* logFile, const EXCEPTION_POINTERS& info);
}
