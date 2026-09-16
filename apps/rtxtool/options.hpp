#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>
#include <osg/Vec3f>

#include <components/rtx/renderer.hpp>

#include "verbs.hpp"

namespace Files
{
    struct ConfigurationManager;
}

namespace RtxTool
{
    /// Which commands read one option.
    struct OptionOwner
    {
        std::string_view mName;
        Verbs mVerbs = Verbs::Every;
    };

    /// Every option the harness takes, and which commands read each. In the library rather than
    /// beside `main`, because a `variables_map` is only usable once notified against the
    /// description that declares its keys, and a test that stands a world up needs the same one.
    struct ToolOptions
    {
        boost::program_options::options_description mDescription;

        /// One entry per option that only some commands read, in the order they are declared: the
        /// same statement the help line is printed from, so a command cannot take an option and
        /// throw it away.
        std::vector<OptionOwner> mOwners;

        /// Which commands read `name`. Every one of them where nothing said otherwise, which is
        /// only the options `Files::ConfigurationManager::addCommonOptions` adds.
        Verbs readsOption(std::string_view name) const;

        /// What `verb` was given on `line` and does not read, as the lines to print, or empty. The
        /// command line only: an option in `openmw.cfg` is there for every command.
        std::string complainAbout(const boost::program_options::parsed_options& line, Verbs verb) const;
    };

    /// `validationByDefault` is what `--validation` reads when nobody names it — a decision about the
    /// command line, which only the executable has: `sync` outside a Release build, `off` in one.
    ToolOptions makeOptions(Rtx::ValidationLevel validationByDefault);

    /// The number `text` spells, or nothing where it spells anything else — the whole of the text,
    /// so `speed = 1500u` is a refusal and not a run that flew at 1500. Read under the classic
    /// locale, because a decimal point read where a comma is the separator flies at one.
    std::optional<float> parseFloat(std::string_view text);

    /// Parses `x,y,z`. Empty text means nothing was said; malformed text throws naming `what`.
    std::optional<osg::Vec3f> parseVec3(std::string_view text, std::string_view what);

    /// Where the engine's own state goes when this tool drives it: the settings it saves on its
    /// way out, its log, its key bindings, its Lua storage. Under the cache path, because every
    /// byte of it is regenerable and the next run overrides it again.
    std::filesystem::path ownConfigDirectory(const Files::ConfigurationManager& config);

    /// Makes `directory` the last configuration directory of the run, creates it, and drops the
    /// settings the last run left in it. The last, because `Settings::Manager::load` reads that
    /// one as the user layer the engine writes back to: with the player's own directory last,
    /// `bench --distant-statics=false` left the played game with its object paging off. Before
    /// `Files::ConfigurationManager::readConfiguration`.
    void adoptConfigDirectory(boost::program_options::variables_map& variables, const std::filesystem::path& directory);
}
