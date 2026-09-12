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

#include "verbs.hpp"

namespace Rtx
{
    struct ValidationOptions;
}

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
        ///
        /// **`makeOptions` leaves nothing to that fallback**, so what reaches it is the four
        /// `Files::ConfigurationManager::addCommonOptions` puts on the same description — and those
        /// really are every command's.
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

    /// The number `text` spells, or nothing where it spells anything else.
    ///
    /// **The whole of the text, so a trailing letter is a refusal** rather than a number and a
    /// shrug: `speed = 1500u` is a typo, and a run that flew at 1500 anyway would report a figure
    /// nobody asked for. `Misc::StringUtils::toNumeric` answers the looser question and takes
    /// whatever prefix parses.
    ///
    /// **Read under the classic locale wherever this runs.** SDL asks the C library for the user's
    /// own on some platforms, and a decimal point read where a comma is the separator turns a view
    /// that flies at 1500 units a second into one that flies at one.
    std::optional<float> parseFloat(std::string_view text);

    /// Parses `x,y,z`. Empty text is not a failure; it means nothing was said.
    ///
    /// Throws `std::runtime_error` naming `what` when the text is present and malformed.
    std::optional<osg::Vec3f> parseVec3(std::string_view text, std::string_view what);

    /// One boolean switch off a command line, and whether anyone actually set it.
    ///
    /// The difference matters: a default is what nobody asked for, and something that was only on
    /// because of a default can be turned off by a flag that contradicts it.
    struct CommandSwitch
    {
        bool mValue = false;
        bool mGiven = false;

        /// True only where it was asked for outright.
        bool isAsked() const { return mValue && mGiven; }

        /// True only where it was turned down outright.
        bool isRefused() const { return !mValue && mGiven; }
    };

    /// Which layers a run wants, from the three switches that can ask for them.
    ///
    /// **An explicit `--validation=false` turns off what was only on by default.** The two finer
    /// switches each imply the layers, and synchronization validation defaults on outside a Release
    /// build — so refusing the layers while leaving that default standing would turn nothing off
    /// at all, and anyone who followed the tool's own advice about timing a frame would measure
    /// one under instrumentation.
    ///
    /// A switch asked for outright still wins: `--validation=false --sync-validation` is a
    /// contradiction, and the more specific half of it is the half that meant something.
    ///
    /// **GPU-assisted validation is never a default**, which is the one asymmetry here and is the
    /// layer's own instruction: it asks at `vkCreateInstance` not to be run beside the core checks.
    /// Left on by the build it did both — a window lost the device at `vkWaitForFences` on three
    /// runs of four, and a headless run aborted inside the layer's own thread. So it is asked for
    /// by name or it is off, and the caller has nothing to decide.
    Rtx::ValidationOptions chooseValidation(CommandSwitch layers, CommandSwitch sync, CommandSwitch gpu);

    /// Where the engine's own state goes when this tool drives it: the settings it saves on its way
    /// out, its log, its key bindings, its Lua storage.
    ///
    /// **Under the cache path, because every byte of it is regenerable.** A run states every setting
    /// it depends on before the engine starts, and a measured run wants nobody's bindings and nobody's
    /// storage — so what the engine writes back is what the next run overrides again.
    std::filesystem::path ownConfigDirectory(const Files::ConfigurationManager& config);

    /// Makes `directory` the last configuration directory of the run, and creates it.
    ///
    /// **Why the last, and why a directory at all.** `Settings::Manager::load` reads `settings.cfg`
    /// from every configuration directory but the last as defaults, and from the last as the user
    /// layer — which is the one `OMW::Engine::go` writes back to. A hosted run sets a dozen settings
    /// before the engine starts, and with the player's own directory last those went into the player's
    /// file: a `bench --distant-statics=false` left the played game with its object paging off. With
    /// this directory last, the player's `settings.cfg` is read as one more layer of defaults and
    /// never written, which is the engine's own `--config` chain used as it is meant to be.
    ///
    /// Appended after whatever `--config` already named, so that it stays last. Must be called before
    /// `Files::ConfigurationManager::readConfiguration`, which is what walks the chain.
    void adoptConfigDirectory(boost::program_options::variables_map& variables, const std::filesystem::path& directory);
}
