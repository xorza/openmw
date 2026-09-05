#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp>

#include "verbs.hpp"

namespace RtxTool
{
    /// Which commands read one option.
    struct OptionOwner
    {
        std::string_view mName;
        Verbs mVerbs = Verbs::Every;
    };

    /// Every option the harness takes, and which commands read each of the ones not all of them do.
    ///
    /// **In the library rather than beside `main`, because standing a world up needs it.** A
    /// `World` is built from a `variables_map`, and a map is only usable once it has been notified
    /// against the description that declares its keys — so anything that reads a cell outside the
    /// tool, a test included, needs this and must not declare a second copy that can drift from it.
    struct ToolOptions
    {
        boost::program_options::options_description mDescription;

        /// One entry per option that only some commands read, in the order they are declared.
        ///
        /// **The same statement the help line is printed from**, which is the point of holding it:
        /// the line said "with `bench`," in prose and nothing checked it, so every other command
        /// took the option and threw it away.
        std::vector<OptionOwner> mOwners;

        /// Which commands read `name`. Every one of them where nothing said otherwise.
        Verbs readsOption(std::string_view name) const;

        /// What `verb` was given on `line` and does not read, as the lines to print — empty where
        /// everything on that line reaches somewhere.
        ///
        /// The command line only: an option in `openmw.cfg` is there for every command, and the
        /// one it is meant for is not the one that has to complain about the rest.
        std::string complainAbout(const boost::program_options::parsed_options& line, Verbs verb) const;
    };

    /// @param validationByDefault what the three validation switches read when nobody names them.
    ///        Passed in rather than compiled in: the build turns the layers on outside a Release
    ///        build, and that is a decision about the *command line*, which only the executable
    ///        downstream of this has.
    ToolOptions makeOptions(bool validationByDefault);
}
