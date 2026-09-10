#include "ownconfig.hpp"

#include <cassert>

#include <boost/program_options/variables_map.hpp>

#include <components/files/configurationmanager.hpp>

namespace RtxTool
{
    namespace
    {
        namespace bpo = boost::program_options;
    }

    std::filesystem::path ownConfigDirectory(const Files::ConfigurationManager& config)
    {
        return config.getCachePath() / "rtxtool";
    }

    void adoptConfigDirectory(bpo::variables_map& variables, const std::filesystem::path& directory)
    {
        std::filesystem::create_directories(directory);

        // **The map's own entry and not a second parse.** `config` is a composing option, so a value
        // stored from a second source would be merged by rules that are Boost's to keep; the container
        // the first parse left is appended to directly, and `readConfiguration` reads it as it finds it.
        //
        // Present whenever the line was parsed against `ConfigurationManager::addCommonOptions`,
        // because `store` writes a defaulted value for every option of its description that the line
        // left out — so a map without it was parsed against the wrong description.
        const auto found = variables.find("config");
        assert(found != variables.end() && "the variables were not parsed against the engine's common options");

        found->second.as<Files::MaybeQuotedPathContainer>().push_back(Files::MaybeQuotedPath{ directory });
    }
}
