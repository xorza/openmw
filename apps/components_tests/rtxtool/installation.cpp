#include "installation.hpp"

#include <filesystem>
#include <string>
#include <vector>

#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>

#include <components/files/configurationmanager.hpp>

#include <apps/rtxtool/options.hpp>
#include <apps/rtxtool/world.hpp>

#include "../rtx/harness.hpp"

namespace RtxTool
{
    namespace bpo = boost::program_options;

    std::unique_ptr<World> openWorld(Files::ConfigurationManager& config, bpo::variables_map& variables)
    {
        bpo::options_description options = makeOptionsDescription(false);
        bpo::store(bpo::command_line_parser(std::vector<std::string>{}).options(options).run(), variables);
        bpo::notify(variables);

        config.processPaths(variables, std::filesystem::current_path());
        config.readConfiguration(variables, options);

        if (variables["content"].as<std::vector<std::string>>().empty())
            return nullptr;

        return std::make_unique<World>(
            config, variables, Rtx::Testing::getShaderDirectory().parent_path().parent_path());
    }
}
