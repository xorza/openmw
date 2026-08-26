#pragma once

namespace ESM
{
    struct Cell;
}

namespace RtxTool
{
    class World;
    struct ActorRequest;
    struct StagingRequest;

    /// Reads a region and reports what the renderer would be handed, without standing one up.
    ///
    /// **Through `StagedWorld`, which is what `shot`, `view` and `bench` trace.** A report assembled
    /// any other way describes a scene nobody renders — and it used to: this built a graph of its
    /// own with nobody standing in it, so it counted a town's geometry and none of its people.
    ///
    /// @param twice walks the same graph a second time and reports what that added, which should be
    ///        nothing. The property the incremental mirror rests on, and the only way to ask it.
    int runScene(
        World& world, const ESM::Cell& cell, const StagingRequest& request, const ActorRequest& actors, bool twice);
}
