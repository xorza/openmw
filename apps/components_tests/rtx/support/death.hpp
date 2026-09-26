#pragma once

#include <functional>
#include <source_location>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#if defined(__linux__)
#include <sys/prctl.h>
#elif !defined(_WIN32)
#include <sys/resource.h>
#endif

namespace Rtx::Testing
{
    /// Leaves the system nothing to keep of this process when it aborts.
    ///
    /// **Non-dumpable on Linux, where a zero core limit is not enough.** With `core_pattern` a pipe
    /// to `systemd-coredump`, the kernel starts the collector for every abort whatever the limit
    /// says, and the collector's start was what a death test cost: five took 964 ms, 252 ms under
    /// a zero limit and 9 ms non-dumpable, which starts nothing and puts nothing in the journal.
    inline void forgoCore()
    {
#if defined(__linux__)
        prctl(PR_SET_DUMPABLE, 0);
#elif !defined(_WIN32)
        const rlimit none{ .rlim_cur = 0, .rlim_max = 0 };
        setrlimit(RLIMIT_CORE, &none);
#endif
    }

    /// Expects `statement` to end the process with `message`, the way a failed `assert` does.
    ///
    /// **The child leaves no core**, by `forgoCore`, and the binary itself still leaves one where
    /// it really crashes.
    ///
    /// @param where the caller's own line, never passed, so a failure reports there.
    inline void expectDies(const std::function<void()>& statement, std::string_view message,
        std::source_location where = std::source_location::current())
    {
        const ::testing::ScopedTrace trace(where.file_name(), static_cast<int>(where.line()), "expectDies");

        EXPECT_DEATH(
            {
                forgoCore();
                statement();
            },
            std::string(message));
    }
}
