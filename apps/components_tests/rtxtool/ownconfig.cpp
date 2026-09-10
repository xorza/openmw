#include <filesystem>
#include <vector>

#include <gtest/gtest.h>

#include <boost/program_options.hpp>

#include <components/files/configurationmanager.hpp>

#include <apps/rtxtool/ownconfig.hpp>

namespace RtxTool
{
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
