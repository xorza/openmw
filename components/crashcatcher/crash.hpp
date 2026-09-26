#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "crashnote.hpp"

/// **The crash catcher**, the same on every system Crashpad supports: Windows, Linux and macOS.
///
/// A monitor process, this same executable started with `--crash-monitor`, reads a crashed,
/// hung or reporting game from outside: Crashpad writes a minidump of every thread, and the
/// monitor appends a summary to the game's log and shows a dialog. Nothing but Crashpad's own
/// signal-safe step runs in the crashed process.
namespace Crash
{
    struct Settings
    {
        /// Names the reports: "OpenMW".
        std::string mApplication;

        /// Where the dumps go, in `crashes/` under it: the log folder.
        std::filesystem::path mReportFolder;

        /// Where the monitor appends each summary: the game's own log.
        std::filesystem::path mLogFile;

        /// Whether a crash and a hang put up a dialog. A harness run from a shell does not want one.
        bool mDialog = true;
    };

    /// Runs the monitor and ends the process, where this process was started as one; returns
    /// otherwise. Called first in `main`, before anything else starts.
    void runMonitorIfAsked(int argc, char** argv);

    /// Starts the monitor and hooks every way this process can end in a crash. Nothing where it did,
    /// and why not where it did not: a system Crashpad does not support, or a monitor that would
    /// not start. Once in a process.
    std::optional<std::string> install(const Settings& settings);

    /// How long without a heartbeat is a hang; nought, as it is until this is called, turns the
    /// check off. The watch begins at the first heartbeat, so a start that draws nothing for a
    /// while is no hang.
    void setHangLimit(std::chrono::seconds limit);

    /// Once for each frame the game draws, a loading screen's included: one relaxed store.
    void heartbeat();

    /// A key and a value every later report carries: the version, the renderer, the device.
    void annotate(std::string_view key, std::string_view value);

    /// A report without a crash, for a contract broken where the game can go on: a dump of every
    /// thread and a summary, and the game continues. Nothing where no catcher is installed.
    void report(std::string_view reason);
}
