#include "benchsuite.hpp"

#include <algorithm>
#include <stdexcept>

#include <components/files/conversion.hpp>
#include <components/rtxbench/benchspec.hpp>
#include <components/settings/categories.hpp>
#include <components/settings/parser.hpp>

namespace RtxTool
{
    std::vector<BenchSuite> loadSuites(const std::filesystem::path& path)
    {
        Settings::CategorySettingValueMap entries;
        Settings::SettingsFileParser parser;

        // The same parser `views.cfg` is read with, for the same reason: the shape is a section per
        // record and a key per field, and that parser already has tests.
        parser.loadSettingsFile(path, entries);

        std::vector<BenchSuite> suites;
        for (const auto& [key, value] : entries)
        {
            const std::string& section = key.first;
            const std::string& field = key.second;

            if (suites.empty() || suites.back().mName != section)
                suites.push_back(BenchSuite{ .mName = section });

            BenchSuite& suite = suites.back();
            if (field == "views")
                suite.mViews = Rtx::splitNames(value);
            else if (field == "note")
                suite.mNote = value;
            else
                throw std::runtime_error("suite \"" + section + "\" has no field called \"" + field + "\"");
        }

        for (const BenchSuite& suite : suites)
            if (suite.mViews.empty())
                throw std::runtime_error("suite \"" + suite.mName + "\" names no views");

        if (suites.empty())
            throw std::runtime_error(Files::pathToUnicodeString(path) + " defines no suites");

        return suites;
    }

    const BenchSuite* findSuite(const std::vector<BenchSuite>& suites, std::string_view name)
    {
        const auto found
            = std::find_if(suites.begin(), suites.end(), [&](const BenchSuite& s) { return s.mName == name; });

        return found == suites.end() ? nullptr : &*found;
    }
}
