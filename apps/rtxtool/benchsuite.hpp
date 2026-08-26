#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace RtxTool
{
    /// A list of places to profile, by name.
    ///
    /// **View ids and not coordinates.** A place worth measuring is a place worth looking at, so a
    /// suite borrows `views.cfg` rather than restating it — which is what keeps the frame a
    /// screenshot shows and the frame a number was measured on the same frame.
    struct BenchSuite
    {
        std::string mName;
        std::string mNote;

        /// In the order they were written, which is the order they are run in.
        std::vector<std::string> mViews;
    };

    /// Reads the suite file. Throws when it is missing or malformed — a mistyped suite should say
    /// so rather than quietly profiling somewhere else.
    std::vector<BenchSuite> loadSuites(const std::filesystem::path& path);

    /// The suite called `name`, or null.
    const BenchSuite* findSuite(const std::vector<BenchSuite>& suites, std::string_view name);

    /// Splits a comma-separated list, dropping the space around each name and any empty entry.
    ///
    /// Shared with `--views`, so a list written on the command line and a list written in the file
    /// are read by the same code and cannot come to disagree about a trailing comma.
    std::vector<std::string> splitNames(std::string_view text);
}
