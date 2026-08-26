#pragma once

#include <filesystem>
#include <string_view>

namespace RtxTool
{
    /// perf's control fifo, so a recording holds the frames that were measured and nothing else.
    ///
    /// `perf record --delay=-1 --control=fifo:<path>` starts with its counters off and turns them
    /// on when the word `enable` arrives on the fifo. Handed that path, this writes the word around
    /// each place's measured frames, and the profile is then those frames: not the engine starting,
    /// not a cell being read off the disk, not the renderer being taken down. Trimming a whole-run
    /// recording to a guessed boundary instead is the alternative, and it reports a quarter of its
    /// samples against code the benchmark never claimed to measure.
    ///
    /// An empty path does nothing at all, which is every run that is not being profiled.
    class PerfControl
    {
    public:
        explicit PerfControl(std::filesystem::path fifo);
        ~PerfControl();

        PerfControl(const PerfControl&) = delete;
        PerfControl& operator=(const PerfControl&) = delete;

        /// Starts counting. The first call opens the fifo.
        void enable();

        /// Stops counting. Silent before the first `enable`, so a run stopped early is not an error.
        void disable();

    private:
        /// Opens the fifo for writing, once.
        ///
        /// **Not on construction, because the reader has to be there first.** Opening a fifo for
        /// writing with nobody reading it fails outright under `O_NONBLOCK` and blocks forever
        /// without it, and perf attaches to an already-running process seconds after it started.
        /// Deferring to the first `enable` puts the open after a cell has been read, by which time
        /// perf has long since opened its end.
        void connect();

        void send(std::string_view command);

        std::filesystem::path mFifo;
        int mHandle = -1;
    };
}
