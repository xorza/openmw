#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Crash
{
    /// What starts this executable as a monitor rather than as the game.
    inline constexpr std::string_view sMonitorSwitch = "--crash-monitor";

    /// **What the game tells its monitor**, on the command line it starts it with: both halves in
    /// one place, so what the game writes is what the monitor reads.
    struct MonitorArguments
    {
        /// The game's process id, which names the page they share.
        std::uint32_t mClient = 0;

        /// Where the note table lies in the game, and how long it is.
        std::uint64_t mNotes = 0;
        std::size_t mNotesSize = 0;

        std::string mApplication;
        bool mDialog = true;
        std::string mIssues;

        /// Where Crashpad keeps the reports: its own `--database`, which the game does not write
        /// and the monitor reads and leaves for Crashpad.
        std::filesystem::path mDatabase;

        /// The arguments the game adds to Crashpad's, the switch first. Paths are UTF-8, which
        /// Crashpad hands on as they are and widens on Windows.
        std::vector<std::string> write() const;

        /// The monitor's arguments out of `arguments`, a whole command line in UTF-8, and the rest
        /// of it, Crashpad's, `argv[0]` first, into `handler`. Nothing of the game's is left in
        /// `handler`, since Crashpad refuses an option it does not know.
        static MonitorArguments read(std::span<const std::string> arguments, std::vector<std::string>& handler);
    };
}
