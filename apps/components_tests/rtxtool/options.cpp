#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <boost/program_options.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>

#include <apps/rtxtool/options.hpp>
#include <apps/rtxtool/run.hpp>
#include <apps/rtxtool/verbs.hpp>
#include <components/files/configurationmanager.hpp>
#include <components/rtx/renderer.hpp>

namespace RtxTool
{
    namespace
    {
        namespace bpo = boost::program_options;

        /// What a command line comes to, parsed against the harness's own description.
        bpo::parsed_options parse(const ToolOptions& options, const std::vector<std::string>& line)
        {
            return bpo::command_line_parser(line).options(options.mDescription).run();
        }

        /// What a switch comes to when nobody names it, which is what `validationByDefault` decides
        /// for two of the three layer switches and for neither of the other one.
        bool defaultOf(const bool validationByDefault, const char* const name)
        {
            const ToolOptions options = makeOptions(validationByDefault);

            bpo::variables_map variables;
            bpo::store(parse(options, {}), variables);
            bpo::notify(variables);

            return variables[name].as<bool>();
        }

        /// GPU-assisted validation is the one layer switch a build never turns on.
        ///
        /// **The layer asks not to be run beside the core checks**, and a build that ran both took
        /// the process down — `RtxTool::chooseValidation` says what that cost. Nothing downstream
        /// can hold this rule: the chooser is handed whatever the description defaulted to, so the
        /// default is where it has to be stated and where it has to be checked.
        TEST(RtxToolOptionsTest, onlyTheGpuAssistedLayerIsNeverOnByDefault)
        {
            EXPECT_TRUE(defaultOf(true, "validation"));
            EXPECT_TRUE(defaultOf(true, "sync-validation"));
            EXPECT_FALSE(defaultOf(true, "gpu-validation")) << "a development build paired it with the core checks";

            // A release build asks for nothing at all, which is the rule the other two follow.
            EXPECT_FALSE(defaultOf(false, "validation"));
            EXPECT_FALSE(defaultOf(false, "sync-validation"));
            EXPECT_FALSE(defaultOf(false, "gpu-validation"));
        }

        /// The bug: an option belonging to one command, given to another, went nowhere.
        ///
        /// `shot --views=balmora` rendered the default view at Seyda Neen and reported it without a
        /// word, because every option is declared on one description and each command reads the
        /// ones it knows about.
        TEST(RtxToolOptionsTest, aCommandRefusesAnOptionItDoesNotRead)
        {
            const ToolOptions options = makeOptions(false);

            EXPECT_EQ(options.complainAbout(parse(options, { "--views=balmora" }), Verbs::Shot),
                "`shot` does not read --views, which belongs to `bench`, `verify` and `check`.\n");

            EXPECT_EQ(options.complainAbout(parse(options, { "--views=balmora" }), Verbs::Bench), "")
                << "the command the option belongs to takes it";
            EXPECT_EQ(options.complainAbout(parse(options, { "--views=balmora" }), Verbs::Verify), "");
            EXPECT_EQ(options.complainAbout(parse(options, { "--views=balmora" }), Verbs::Check), "");

            // The same mistake the other way round: a run of places takes its cell from `--views`,
            // and `--view` is what a command that stands at one place reads.
            EXPECT_EQ(options.complainAbout(parse(options, { "--view=balmora" }), Verbs::Bench),
                "`bench` does not read --view, which belongs to every command but `info`, `bench`, `verify` "
                "and `check`.\n");
            EXPECT_EQ(options.complainAbout(parse(options, { "--view=balmora" }), Verbs::Shot), "");
        }

        /// Every option on the line is answered for, and each of them once.
        TEST(RtxToolOptionsTest, aLineIsAnsweredForOptionByOption)
        {
            const ToolOptions options = makeOptions(false);

            // Two the command does not read, around one it does and one nobody restricted.
            const bpo::parsed_options line
                = parse(options, { "--suite=default", "--upscale=off", "--find=barrel", "--seconds=4" });

            EXPECT_EQ(
                options.complainAbout(line, Verbs::Bench), "`bench` does not read --find, which belongs to `scene`.\n");
            EXPECT_EQ(options.complainAbout(line, Verbs::Scene),
                "`scene` does not read --suite, which belongs to `bench` and `check`.\n"
                "`scene` does not read --seconds, which belongs to `bench` and `check`.\n");

            // A composing option is written once per value and is worth one complaint.
            EXPECT_EQ(options.complainAbout(parse(options, { "--npc=fargoth", "--npc=hrisskar" }), Verbs::Doll), "")
                << "the doll is one person out of --npc";

            const bpo::parsed_options twice = parse(options, { "--out=a.png", "--out=b.png" });
            EXPECT_EQ(options.complainAbout(twice, Verbs::Bench),
                "`bench` does not read --out, which belongs to `shot`, `textures`, `doll`, `map` and `verify`.\n");
        }

        /// Every option says which commands read it, and the ones that say "all of them" say it.
        ///
        /// The bug this holds shut: a knob that never named an owner answered `Verbs::Every` by
        /// default, so `info --size=800x600 --delight=0` was taken and thrown away.
        TEST(RtxToolOptionsTest, everyOptionSaysWhichCommandsReadIt)
        {
            const ToolOptions options = makeOptions(false);

            // Upstream's own — `--config` and its three siblings — reach the same description
            // through `Files::ConfigurationManager` and are every command's by nature. They are the
            // only names `readsOption` is allowed to answer by falling through.
            bpo::options_description upstream("");
            Files::ConfigurationManager::addCommonOptions(upstream);

            std::set<std::string> owned;
            for (const OptionOwner& owner : options.mOwners)
                owned.insert(std::string(owner.mName));

            for (const auto& declared : options.mDescription.options())
            {
                const std::string& name = declared->long_name();
                const bool theirs = upstream.find_nothrow(name, false) != nullptr;

                EXPECT_TRUE(theirs || owned.contains(name)) << name << " reached the description with no owner";
            }

            EXPECT_EQ(options.readsOption("validation"), Verbs::Every);
            EXPECT_EQ(options.readsOption("data"), Verbs::Every) << "the engine's own, read by every command";
            EXPECT_EQ(options.readsOption("views"), Verbs::Bench | Verbs::Verify | Verbs::Check);

            // Every command but `info` builds a frame, and `info` reports on a device.
            EXPECT_EQ(options.readsOption("upscale"), otherThan(Verbs::Info));
            EXPECT_EQ(options.readsOption("size"), otherThan(Verbs::Info));
            EXPECT_EQ(options.complainAbout(parse(options, { "--size=8x8" }), Verbs::Info),
                "`info` does not read --size, which belongs to every command but `info`.\n");
            EXPECT_EQ(options.complainAbout(parse(options, { "--size=8x8" }), Verbs::Doll), "")
                << "`doll` frames a camera through the same request every other command does";

            for (const std::string_view name :
                { "info", "scene", "shot", "view", "bench", "textures", "doll", "map", "verify", "check" })
                EXPECT_EQ(options.complainAbout(parse(options, { "--validation=false" }), verbNamed(name)), "") << name;
        }

        /// The help line and the check are one statement, so a reader is told what the tool
        /// enforces.
        TEST(RtxToolOptionsTest, anOwnedOptionSaysSoInItsHelpLine)
        {
            const ToolOptions options = makeOptions(false);

            const auto lineFor
                = [&](const std::string& name) { return options.mDescription.find(name, false).description(); };

            EXPECT_TRUE(lineFor("views").starts_with("with `bench`, `verify` and `check`, ")) << lineFor("views");
            EXPECT_TRUE(lineFor("find").starts_with("with `scene`, ")) << lineFor("find");

            // Nine of the ten read a camera, so the line names the one that does not rather than
            // the nine that do.
            EXPECT_TRUE(lineFor("fov").starts_with("with every command but `info`, ")) << lineFor("fov");

            EXPECT_FALSE(lineFor("validation").starts_with("with ")) << "nothing to say where every command reads it";
        }

        /// The names the two tables share: an option's owner and the dispatch's row are the same
        /// word for the same command.
        TEST(RtxVerbsTest, everyCommandHasOneNameAndOneBit)
        {
            EXPECT_EQ(verbName(Verbs::Shot), "shot");
            EXPECT_EQ(verbNamed("shot"), Verbs::Shot);
            EXPECT_EQ(verbName(Verbs::Check), "check");
            EXPECT_EQ(verbNamed("check"), Verbs::Check);
            EXPECT_EQ(verbNamed("nonesuch"), Verbs::None);
            EXPECT_EQ(verbName(Verbs::Bench | Verbs::Verify), "") << "a set of two is not a command";
            EXPECT_EQ(verbName(Verbs::None), "");

            EXPECT_EQ(countVerbs(Verbs::Every), 10u) << "the ten `--help` prints";
            EXPECT_EQ(countVerbs(Verbs::None), 0u);
            EXPECT_EQ(otherThan(Verbs::Every), Verbs::None);
            EXPECT_EQ(countVerbs(otherThan(Verbs::Shot)), 9u);
            EXPECT_TRUE(holds(Verbs::Bench | Verbs::Verify, Verbs::Verify));
            EXPECT_FALSE(holds(Verbs::Bench | Verbs::Verify, Verbs::Shot));

            EXPECT_EQ(describeVerbs(Verbs::Shot), "`shot`");
            EXPECT_EQ(describeVerbs(Verbs::Bench | Verbs::Verify), "`bench` and `verify`");
            EXPECT_EQ(describeVerbs(Verbs::Scene | Verbs::Shot | Verbs::Map), "`scene`, `shot` and `map`")
                << "in the order --help prints them, whatever order they were written in";
            EXPECT_EQ(describeVerbs(Verbs::None), "");
        }
    }

    namespace
    {
        /// What the three switches look like when nobody has said anything, outside a Release build.
        constexpr CommandSwitch sDefaultOn{ .mValue = true, .mGiven = false };
        constexpr CommandSwitch sAsked{ .mValue = true, .mGiven = true };
        constexpr CommandSwitch sRefused{ .mValue = false, .mGiven = true };

        /// What a switch with no build default looks like, which `--gpu-validation` always is and
        /// every switch is in a Release build.
        constexpr CommandSwitch sQuiet{ .mValue = false, .mGiven = false };

        /// Left alone, a development build loads the layers and the synchronization checks.
        ///
        /// **And never the GPU-assisted one**, which the layer itself asks not to be run beside the
        /// core checks: `--gpu-validation` has no build default to carry it, so nothing but the name
        /// turns it on. `chooseValidation`'s own header says what the pairing cost.
        TEST(RtxValidationChoiceTest, theGpuAssistedLayerArrivesByNameAndNoOtherWay)
        {
            const Rtx::ValidationOptions defaults = chooseValidation(sDefaultOn, sDefaultOn, sQuiet);
            EXPECT_TRUE(defaults.mEnabled);
            EXPECT_TRUE(defaults.mSynchronization);
            EXPECT_FALSE(defaults.mGpuAssisted);

            EXPECT_TRUE(chooseValidation(sDefaultOn, sDefaultOn, sAsked).mGpuAssisted) << "asking did not turn it on";

            // **And not from a default either**, which is what reading this one by name buys: a
            // build that started handing one out could not pair the two again by accident.
            EXPECT_FALSE(chooseValidation(sDefaultOn, sDefaultOn, sDefaultOn).mGpuAssisted);
        }

        /// The bug this rule was written for: refusing the layers has to turn them off.
        ///
        /// Both finer switches imply the layers and both default on, so leaving their defaults
        /// standing meant `--validation=false` changed nothing at all — and the tool told anyone
        /// timing a frame to pass exactly that.
        TEST(RtxValidationChoiceTest, refusingTheLayersTurnsOffWhatWasOnlyOnByDefault)
        {
            const Rtx::ValidationOptions off = chooseValidation(sRefused, sDefaultOn, sQuiet);
            EXPECT_FALSE(off.mEnabled);
            EXPECT_FALSE(off.mSynchronization);
            EXPECT_FALSE(off.mGpuAssisted);
        }

        /// A switch asked for by name beats a blanket refusal, which is the more specific request
        /// winning rather than the later one.
        TEST(RtxValidationChoiceTest, aSwitchAskedForByNameSurvivesARefusalOfTheRest)
        {
            const Rtx::ValidationOptions sync = chooseValidation(sRefused, sAsked, sQuiet);
            EXPECT_TRUE(sync.mSynchronization);
            EXPECT_FALSE(sync.mGpuAssisted) << "still only on by default, and still refused";
            EXPECT_TRUE(sync.mEnabled) << "synchronization validation implies the layer that carries it";

            const Rtx::ValidationOptions gpu = chooseValidation(sRefused, sDefaultOn, sAsked);
            EXPECT_TRUE(gpu.mGpuAssisted);
            EXPECT_FALSE(gpu.mSynchronization);
            EXPECT_TRUE(gpu.mEnabled);
        }

        /// Every request that stands a renderer up hands the choice on.
        ///
        /// **The bug this was written for: `doll` and `map` dropped it.** They built their options
        /// inline and named every field but the layers, so the two commands parsed
        /// `--sync-validation`, accepted it, and traced with nothing loaded — and a run under it came
        /// back clean because nothing was checking. The switch is now an argument of the conversion
        /// rather than a field of the request, so a caller has to have one in hand.
        TEST(RtxValidationChoiceTest, aReleaseBuildStaysQuietUntilSomethingIsAskedFor)
        {
            EXPECT_FALSE(chooseValidation(sQuiet, sQuiet, sQuiet).mEnabled);
            EXPECT_TRUE(chooseValidation(sAsked, sQuiet, sQuiet).mEnabled);
            EXPECT_TRUE(chooseValidation(sQuiet, sAsked, sQuiet).mEnabled);
            EXPECT_TRUE(chooseValidation(sQuiet, sQuiet, sAsked).mEnabled);
        }

        /// A run that named a layer demands it; a build that switched one on does not.
        ///
        /// **The difference decides whether a missing layer stops the run.** Without the layers
        /// nothing reports, so a gate that asked for them and got none reads an empty log as a pass
        /// — while a developer whose build turned them on by default still wants a renderer that
        /// starts. `Rtx::ValidationOptions::mDemanded` is what tells the two apart.
        TEST(RtxValidationChoiceTest, onlyASwitchNamedOnTheCommandLineDemandsTheLayers)
        {
            EXPECT_FALSE(chooseValidation(sDefaultOn, sDefaultOn, sQuiet).mDemanded)
                << "a build default demanded the layers";
            EXPECT_FALSE(chooseValidation(sRefused, sQuiet, sQuiet).mDemanded)
                << "turning the layers down demanded them";

            EXPECT_TRUE(chooseValidation(sAsked, sQuiet, sQuiet).mDemanded);
            EXPECT_TRUE(chooseValidation(sQuiet, sAsked, sQuiet).mDemanded);
            EXPECT_TRUE(chooseValidation(sQuiet, sQuiet, sAsked).mDemanded);

            // A demand for the finer layer stands even against a refusal of the coarser one, which
            // is the same rule `mEnabled` follows above.
            EXPECT_TRUE(chooseValidation(sRefused, sAsked, sQuiet).mDemanded);
        }
    }

    namespace
    {
        namespace bpo = boost::program_options;

        bpo::variables_map parse(const std::vector<const char*>& arguments)
        {
            bpo::options_description description;
            Files::ConfigurationManager::addCommonOptions(description);

            bpo::variables_map variables;
            bpo::store(bpo::command_line_parser(static_cast<int>(arguments.size()), arguments.data())
                           .options(description)
                           .run(),
                variables);
            bpo::notify(variables);
            return variables;
        }

        std::vector<std::filesystem::path> configDirectories(const bpo::variables_map& variables)
        {
            return Files::asPathContainer(variables.at("config").as<Files::MaybeQuotedPathContainer>());
        }

        /// The directory is created, and it is the last one whatever `--config` named before it —
        /// because the last configuration directory is the one the engine writes into.
        TEST(RtxOwnConfigTest, theDirectoryIsMadeAndComesLast)
        {
            const std::filesystem::path own = std::filesystem::temp_directory_path() / "openmw-rtx-own-config-test";
            std::filesystem::remove_all(own);

            bpo::variables_map bare = parse({ "openmw-rtxtool" });
            adoptConfigDirectory(bare, own);
            EXPECT_TRUE(std::filesystem::is_directory(own)) << "made, because the engine writes into it";
            EXPECT_EQ(configDirectories(bare), std::vector<std::filesystem::path>{ own });

            bpo::variables_map named = parse({ "openmw-rtxtool", "--config", "/one", "--config", "/two" });
            adoptConfigDirectory(named, own);
            EXPECT_EQ(configDirectories(named), (std::vector<std::filesystem::path>{ "/one", "/two", own }))
                << "after what the command line named, so it is the one the engine saves into";

            std::filesystem::remove_all(own);
        }
    }
}
