#pragma once

#include <filesystem>

#include <boost/program_options/variables_map.hpp>

namespace Files
{
    struct ConfigurationManager;
}

namespace RtxTool
{
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
