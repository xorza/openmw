#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>

#include <apps/rtxtool/options.hpp>
#include <apps/rtxtool/run.hpp>
#include <apps/rtxtool/verbs.hpp>
#include <components/files/configurationmanager.hpp>
#include <components/rtx/renderer.hpp>
#include <components/testing/util.hpp>

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

        /// What `--validation` comes to when nobody names it, which is what `makeOptions` is told.
        std::string defaultValidation(const Rtx::ValidationLevel byDefault)
        {
            const ToolOptions options = makeOptions(byDefault);

            bpo::variables_map variables;
            bpo::store(parse(options, {}), variables);
            bpo::notify(variables);

            return variables["validation"].as<std::string>();
        }

        /// The build decides the level nobody named, and the level is spelled the way the table
        /// spells it, so a script that names `sync` and a build that defaults to it agree.
        TEST(RtxToolOptionsTest, theBuildDecidesTheValidationLevelNobodyNamed)
        {
            EXPECT_EQ(defaultValidation(Rtx::ValidationLevel::Sync), "sync");
            EXPECT_EQ(defaultValidation(Rtx::ValidationLevel::Off), "off");
        }

        /// The bug: an option belonging to one command, given to another, went nowhere.
        ///
        /// `shot --views=balmora` rendered the default view at Seyda Neen and reported it without a
        /// word, because every option is declared on one description and each command reads the
        /// ones it knows about.
        TEST(RtxToolOptionsTest, aCommandRefusesAnOptionItDoesNotRead)
        {
            const ToolOptions options = makeOptions(Rtx::ValidationLevel::Off);

            EXPECT_EQ(options.complainAbout(parse(options, { "--views=balmora" }), Verbs::View),
                "`view` does not read --views, which belongs to every command but `info`, `view` and `film`.\n");

            EXPECT_EQ(options.complainAbout(parse(options, { "--views=balmora" }), Verbs::Bench), "")
                << "the command the option belongs to takes it";
            EXPECT_EQ(options.complainAbout(parse(options, { "--views=balmora" }), Verbs::Shot), "");
            EXPECT_EQ(options.complainAbout(parse(options, { "--views=balmora" }), Verbs::Check), "");

            // The same mistake the other way round: a run of places takes its cell from `--views`,
            // and `--view` is what a command that stands at one place reads.
            EXPECT_EQ(options.complainAbout(parse(options, { "--view=balmora" }), Verbs::Bench),
                "`bench` does not read --view, which belongs to `scene`, `shot` and `view`.\n");
            EXPECT_EQ(options.complainAbout(parse(options, { "--view=balmora" }), Verbs::Shot), "");

            // A film's keys each name their own sky, so one sky on the line would be thrown away.
            EXPECT_EQ(options.complainAbout(parse(options, { "--hour=6" }), Verbs::Film),
                "`film` does not read --hour, which belongs to every command but `info` and `film`.\n");
            EXPECT_EQ(options.complainAbout(parse(options, { "--keys=tour.keys" }), Verbs::View), "")
                << "a window writes the keys a film reads";
            EXPECT_EQ(options.complainAbout(parse(options, { "--keys=tour.keys", "--day=3" }), Verbs::Film), "");
        }

        /// Every option on the line is answered for, and each of them once.
        TEST(RtxToolOptionsTest, aLineIsAnsweredForOptionByOption)
        {
            const ToolOptions options = makeOptions(Rtx::ValidationLevel::Off);

            // Two the command does not read, around one it does and one nobody restricted.
            const bpo::parsed_options line
                = parse(options, { "--suite=default", "--upscale=off", "--find=barrel", "--seconds=4" });

            EXPECT_EQ(
                options.complainAbout(line, Verbs::Bench), "`bench` does not read --find, which belongs to `scene`.\n");
            EXPECT_EQ(options.complainAbout(line, Verbs::Scene),
                "`scene` does not read --suite, which belongs to `bench` and `check`.\n"
                "`scene` does not read --seconds, which belongs to `bench` and `check`.\n");

            // An option written twice is worth one complaint.
            const bpo::parsed_options twice = parse(options, { "--out=a", "--out=b" });
            EXPECT_EQ(options.complainAbout(twice, Verbs::Bench),
                "`bench` does not read --out, which belongs to `shot`, `check` and `film`.\n");
        }

        /// Every option says which commands read it, and the ones that say "all of them" say it.
        ///
        /// The bug this holds shut: a knob that never named an owner answered `Verbs::Every` by
        /// default, so `info --size=800x600 --delight=0` was taken and thrown away.
        TEST(RtxToolOptionsTest, everyOptionSaysWhichCommandsReadIt)
        {
            const ToolOptions options = makeOptions(Rtx::ValidationLevel::Off);

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
            EXPECT_EQ(options.readsOption("views"), Verbs::Scene | Verbs::Shot | Verbs::Bench | Verbs::Check);

            // Every command but `info` builds a frame, and `info` reports on a device.
            EXPECT_EQ(options.readsOption("upscale"), otherThan(Verbs::Info));
            EXPECT_EQ(options.readsOption("size"), otherThan(Verbs::Info));
            EXPECT_EQ(options.complainAbout(parse(options, { "--size=8x8" }), Verbs::Info),
                "`info` does not read --size, which belongs to every command but `info`.\n");
            EXPECT_EQ(options.complainAbout(parse(options, { "--size=8x8" }), Verbs::Check), "")
                << "`check` frames a camera through the same request every other command does";

            for (const std::string_view name : { "info", "scene", "shot", "view", "bench", "check", "film" })
                EXPECT_EQ(options.complainAbout(parse(options, { "--validation=off" }), verbNamed(name)), "") << name;
        }

        /// The help line and the check are one statement, so a reader is told what the tool
        /// enforces.
        TEST(RtxToolOptionsTest, anOwnedOptionSaysSoInItsHelpLine)
        {
            const ToolOptions options = makeOptions(Rtx::ValidationLevel::Off);

            const auto lineFor
                = [&](const std::string& name) { return options.mDescription.find(name, false).description(); };

            EXPECT_TRUE(lineFor("views").starts_with("with every command but `info`, `view` and `film`, "))
                << lineFor("views");
            EXPECT_TRUE(lineFor("find").starts_with("with `scene`, ")) << lineFor("find");

            // Six of the seven read a camera, so the line names the one that does not rather than
            // the six that do.
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
            EXPECT_EQ(verbName(Verbs::Film), "film");
            EXPECT_EQ(verbNamed("film"), Verbs::Film);
            EXPECT_EQ(verbNamed("nonesuch"), Verbs::None);
            EXPECT_EQ(verbName(Verbs::Bench | Verbs::Check), "") << "a set of two is not a command";
            EXPECT_EQ(verbName(Verbs::None), "");

            EXPECT_EQ(countVerbs(Verbs::Every), 7u) << "the seven `--help` prints";
            EXPECT_EQ(countVerbs(Verbs::None), 0u);
            EXPECT_EQ(otherThan(Verbs::Every), Verbs::None);
            EXPECT_EQ(countVerbs(otherThan(Verbs::Shot)), 6u);
            EXPECT_TRUE(holds(Verbs::Bench | Verbs::Check, Verbs::Check));
            EXPECT_FALSE(holds(Verbs::Bench | Verbs::Check, Verbs::Shot));

            EXPECT_EQ(describeVerbs(Verbs::Shot), "`shot`");
            EXPECT_EQ(describeVerbs(Verbs::Bench | Verbs::Check), "`bench` and `check`");
            EXPECT_EQ(describeVerbs(Verbs::Check | Verbs::Shot | Verbs::Scene), "`scene`, `shot` and `check`")
                << "in the order --help prints them, whatever order they were written in";
            EXPECT_EQ(describeVerbs(Verbs::None), "");
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
            const std::filesystem::path own = TestingOpenMW::outputFilePath("own-config-test");
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
