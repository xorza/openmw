#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <boost/program_options/parsers.hpp>

#include <apps/rtxtool/options.hpp>
#include <apps/rtxtool/verbs.hpp>

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

        /// The bug: an option belonging to one command, given to another, went nowhere.
        ///
        /// `shot --views=balmora` rendered the default view at Seyda Neen and reported it without a
        /// word, because every option is declared on one description and each command reads the
        /// ones it knows about.
        TEST(RtxToolOptionsTest, aCommandRefusesAnOptionItDoesNotRead)
        {
            const ToolOptions options = makeOptions(false);

            EXPECT_EQ(options.complainAbout(parse(options, { "--views=balmora" }), Verbs::Shot),
                "`shot` does not read --views, which belongs to `bench` and `verify`.\n");

            EXPECT_EQ(options.complainAbout(parse(options, { "--views=balmora" }), Verbs::Bench), "")
                << "the command the option belongs to takes it";
            EXPECT_EQ(options.complainAbout(parse(options, { "--views=balmora" }), Verbs::Verify), "");

            // The same mistake the other way round: a run of places takes its cell from `--views`,
            // and `--view` is what a command that stands at one place reads.
            EXPECT_EQ(options.complainAbout(parse(options, { "--view=balmora" }), Verbs::Bench),
                "`bench` does not read --view, which belongs to `scene`, `shot`, `view`, `textures` and `map`.\n");
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
                "`scene` does not read --suite, which belongs to `bench`.\n"
                "`scene` does not read --seconds, which belongs to `bench`.\n");

            // A composing option is written once per value and is worth one complaint.
            EXPECT_EQ(options.complainAbout(parse(options, { "--npc=fargoth", "--npc=hrisskar" }), Verbs::Doll), "")
                << "the doll is one person out of --npc";

            const bpo::parsed_options twice = parse(options, { "--out=a.png", "--out=b.png" });
            EXPECT_EQ(options.complainAbout(twice, Verbs::Bench),
                "`bench` does not read --out, which belongs to `shot`, `textures`, `doll`, `map` and `verify`.\n");
        }

        /// An option nobody restricted is every command's, and a command line that names none of
        /// the restricted ones is nobody's complaint.
        TEST(RtxToolOptionsTest, anUnrestrictedOptionIsEveryCommandsToRead)
        {
            const ToolOptions options = makeOptions(false);

            EXPECT_EQ(options.readsOption("upscale"), Verbs::Every);
            EXPECT_EQ(options.readsOption("validation"), Verbs::Every);
            EXPECT_EQ(options.readsOption("data"), Verbs::Every) << "nothing declared here is restricted either";
            EXPECT_EQ(options.readsOption("views"), Verbs::Bench | Verbs::Verify);

            for (const std::string_view name :
                { "info", "scene", "shot", "view", "bench", "textures", "doll", "map", "verify" })
                EXPECT_EQ(options.complainAbout(parse(options, { "--upscale=off" }), verbNamed(name)), "") << name;
        }

        /// The help line and the check are one statement, so a reader is told what the tool
        /// enforces.
        TEST(RtxToolOptionsTest, anOwnedOptionSaysSoInItsHelpLine)
        {
            const ToolOptions options = makeOptions(false);

            const auto lineFor
                = [&](const std::string& name) { return options.mDescription.find(name, false).description(); };

            EXPECT_TRUE(lineFor("views").starts_with("with `bench` and `verify`, ")) << lineFor("views");
            EXPECT_TRUE(lineFor("find").starts_with("with `scene`, ")) << lineFor("find");

            // Most of the nine read a camera, so the line names the few that do not rather than
            // the many that do.
            EXPECT_TRUE(lineFor("fov").starts_with("with every command but `info` and `doll`, ")) << lineFor("fov");

            EXPECT_FALSE(lineFor("upscale").starts_with("with ")) << "nothing to say about a command that reads it";
        }

        /// The names the two tables share: an option's owner and the dispatch's row are the same
        /// word for the same command.
        TEST(RtxVerbsTest, everyCommandHasOneNameAndOneBit)
        {
            EXPECT_EQ(verbName(Verbs::Shot), "shot");
            EXPECT_EQ(verbNamed("shot"), Verbs::Shot);
            EXPECT_EQ(verbNamed("nonesuch"), Verbs::None);
            EXPECT_EQ(verbName(Verbs::Bench | Verbs::Verify), "") << "a set of two is not a command";
            EXPECT_EQ(verbName(Verbs::None), "");

            EXPECT_EQ(countVerbs(Verbs::Every), 9u) << "the nine `--help` prints";
            EXPECT_EQ(countVerbs(Verbs::None), 0u);
            EXPECT_EQ(otherThan(Verbs::Every), Verbs::None);
            EXPECT_EQ(countVerbs(otherThan(Verbs::Shot)), 8u);
            EXPECT_TRUE(holds(Verbs::Bench | Verbs::Verify, Verbs::Verify));
            EXPECT_FALSE(holds(Verbs::Bench | Verbs::Verify, Verbs::Shot));

            EXPECT_EQ(describeVerbs(Verbs::Shot), "`shot`");
            EXPECT_EQ(describeVerbs(Verbs::Bench | Verbs::Verify), "`bench` and `verify`");
            EXPECT_EQ(describeVerbs(Verbs::Scene | Verbs::Shot | Verbs::Map), "`scene`, `shot` and `map`")
                << "in the order --help prints them, whatever order they were written in";
            EXPECT_EQ(describeVerbs(Verbs::None), "");
        }
    }
}
