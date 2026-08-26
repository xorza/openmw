#pragma once

#include <filesystem>

namespace ESM
{
    struct Cell;
}

namespace RtxTool
{
    class World;
    struct ActorRequest;
    struct StagingRequest;

    /// Every texture a cell uses, vanilla beside de-lit, written as one sheet.
    ///
    /// **Off the same staged region the renderer is handed**, so the sheet holds what a frame of
    /// that cell would actually sample — a town's people wear textures the town itself never names.
    ///
    /// @param strength how much of the lighting painted into each texture to divide back out, which
    ///        is the estimate the trace makes and the only way to look at it directly.
    int runTextures(World& world, const ESM::Cell& cell, const StagingRequest& request, const ActorRequest& actors,
        const std::filesystem::path& output, float strength);
}
