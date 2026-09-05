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
}
