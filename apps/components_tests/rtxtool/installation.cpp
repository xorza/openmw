#include "installation.hpp"

#include <filesystem>
#include <string>
#include <vector>

#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>

#include <apps/rtxtool/content.hpp>
#include <apps/rtxtool/options.hpp>
#include <apps/rtxtool/world.hpp>
#include <components/files/configurationmanager.hpp>

#include "../rtx/harness.hpp"

namespace RtxTool
{
    namespace
    {
        namespace bpo = boost::program_options;

        /// Opens the installation the tool would open, or says why there is none.
        ///
        /// **The same route the tool takes and not a second one**: the configuration manager reads
        /// `openmw.cfg`, which is what says where the game is installed and which content files to
        /// merge. A machine without the game is a legitimate skip; a machine with it configured
        /// wrongly is a failure, and that comes out of `Content`'s own constructor.
        ///
        /// The configuration and the parsed command line are local because `Content` reads them and
        /// does not hold them.
        std::unique_ptr<Content> buildContent(std::string& reason)
        {
            Files::ConfigurationManager config;
            bpo::variables_map variables;

            bpo::options_description options = makeOptionsDescription(false);
            bpo::store(bpo::command_line_parser(std::vector<std::string>{}).options(options).run(), variables);
            bpo::notify(variables);

            config.processPaths(variables, std::filesystem::current_path());
            config.readConfiguration(variables, options);

            if (variables["content"].as<std::vector<std::string>>().empty())
            {
                reason = "no Morrowind installation is configured: openmw.cfg names no content file";
                return nullptr;
            }

            return std::make_unique<Content>(
                config, variables, Rtx::Testing::getShaderDirectory().parent_path().parent_path());
        }

        /// The installation for the whole test binary. See `Rtx::Testing::Once`, which the device
        /// harness holds its instance and its renderer in for the same reason.
        Rtx::Testing::Once<Content>& contentCache()
        {
            static Rtx::Testing::Once<Content> sContent;
            return sContent;
        }

        /// Closes the installation after the last test and before `main` returns.
        ///
        /// A cache that lives for the run should end with the run: the content holds a reader open
        /// on every file it merged, and static destruction is nobody's idea of where to give those
        /// back.
        class ContentTeardown : public ::testing::Environment
        {
            void TearDown() override
            {
                contentCache().release("the suite closed the installation after the last test");
            }
        };

        // Before `main`, because gtest only tears down environments registered before the run starts.
        [[maybe_unused]] const bool sRegistered = [] {
            ::testing::AddGlobalTestEnvironment(new ContentTeardown);
            return true;
        }();
    }

    void InstallationTest::SetUp()
    {
        std::string reason;
        mContent = contentCache().get(reason, buildContent);

        if (mContent == nullptr)
            GTEST_SKIP() << reason;
    }

    std::unique_ptr<World> InstallationTest::openWorld() const
    {
        return std::make_unique<World>(*mContent);
    }

    World& InstallationTest::getWorld()
    {
        if (mWorld == nullptr)
            mWorld = openWorld();

        return *mWorld;
    }
}
