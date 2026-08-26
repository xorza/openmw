#pragma once

#include <memory>

namespace boost::program_options
{
    class variables_map;
}

namespace Files
{
    struct ConfigurationManager;
}

namespace RtxTool
{
    class World;

    /// A Morrowind installation as the harness finds one, or null where there is none.
    ///
    /// **The same route the tool takes and not a second one**: the configuration manager reads
    /// `openmw.cfg`, which is what says where the game is installed and which content files to
    /// merge. A machine without the game is a legitimate skip; a machine with it configured wrongly
    /// is a failure, and that comes out of `World`'s own constructor.
    ///
    /// `config` and `variables` are borrowed for the world's whole life — it holds references into
    /// both — so they belong to the test and not to this.
    std::unique_ptr<World> openWorld(
        Files::ConfigurationManager& config, boost::program_options::variables_map& variables);
}
