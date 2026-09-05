#pragma once

#include <filesystem>

#include <boost/program_options/variables_map.hpp>

#include <apps/openmw/mwrender/rtx/session.hpp>

namespace Files
{
    struct ConfigurationManager;
}

namespace RtxTool
{
    /// Runs `request` against a real game, headless, and gives back what it came to.
    ///
    /// **The game and not a world of this tool's own.** A staged world re-walks only its actors, so
    /// it never pays for the whole-graph walk, the sweep, or a cell arriving — the three things
    /// that actually cost a frame. What it also cannot do is stand in the world the player stands
    /// in: its cells are read by hand, its people are dressed by rules of this tool's own, and its
    /// sky is derived from the content files rather than reported by a weather system. So a
    /// picture taken here and a picture played were two pictures.
    ///
    /// **The engine is built exactly as `apps/openmw/main.cpp` builds one**, out of the same option
    /// table, so a run reaches the same content through the same loader. What this adds is a
    /// schedule and the switches a measurement needs, and nothing else.
    ///
    /// @param variables the parsed command line, which carries the data directories, the content
    ///        files and the encoding the engine is configured from.
    /// @return a process exit status.
    int runHosted(const boost::program_options::variables_map& variables, Files::ConfigurationManager& config,
        const std::filesystem::path& resources, MWRender::SessionRequest request);
}
