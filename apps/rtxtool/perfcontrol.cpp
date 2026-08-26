#include "perfcontrol.hpp"

#include <cerrno>
#include <cstring>
#include <format>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#include <components/files/conversion.hpp>
#include <components/rtx/error.hpp>

namespace RtxTool
{
    PerfControl::PerfControl(std::filesystem::path fifo)
        : mFifo(std::move(fifo))
    {
    }

    PerfControl::~PerfControl()
    {
        if (mHandle >= 0)
            ::close(mHandle);
    }

    void PerfControl::enable()
    {
        if (mFifo.empty())
            return;

        connect();
        send("enable\n");
    }

    void PerfControl::disable()
    {
        if (mHandle < 0)
            return;

        send("disable\n");
    }

    void PerfControl::connect()
    {
        if (mHandle >= 0)
            return;

        // `O_NONBLOCK` on a write-only fifo is what turns "no reader" from a hang into `ENXIO`, and
        // a profiling run that hung would look like the benchmark being slow.
        mHandle = ::open(mFifo.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (mHandle < 0)
            throw Rtx::Error(std::format("cannot write to the perf control fifo {}: {}",
                Files::pathToUnicodeString(mFifo), std::strerror(errno)));
    }

    void PerfControl::send(std::string_view command)
    {
        const ssize_t written = ::write(mHandle, command.data(), command.size());
        if (written != static_cast<ssize_t>(command.size()))
            throw Rtx::Error(std::format("cannot send `{}` to the perf control fifo {}: {}",
                command.substr(0, command.size() - 1), Files::pathToUnicodeString(mFifo), std::strerror(errno)));
    }
}
