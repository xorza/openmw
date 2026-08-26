#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

#include <osg/Vec3f>

namespace ESM
{
    struct Cell;
    struct NPC;
}

namespace RtxTool
{
    class World;
    struct ActorRequest;
    struct StagingRequest;

    /// What a picture inside the interface needs beyond what it is a picture of.
    ///
    /// **The same two the game asks for**, and the reason these commands exist: the inventory doll
    /// and the local map are the one part of a frame the harness could not draw, so every defect in
    /// them had to be found by opening the game and looking. `Rtx::OffscreenTrace` is the half both
    /// now share.
    struct PictureRequest
    {
        std::filesystem::path mOutput;
        std::filesystem::path mShaderDirectory;

        std::uint32_t mWidth = 512;
        std::uint32_t mHeight = 1024;

        /// Where in the idle to catch them, in the track's own seconds. A doll only.
        float mSeconds = 0.0f;

        /// Whether the person has their clothes on. A doll only; skin is the harder surface.
        bool mDressed = true;

        /// Where to stand and what to look at, or nothing for the framing the game uses. A doll
        /// only: a map tile is straight down over the cell by construction.
        std::optional<osg::Vec3f> mOrigin;
        std::optional<osg::Vec3f> mTarget;
    };

    /// The inventory doll, traced against a scene of its own and written as a PNG.
    ///
    /// **A subject mirrored into its own scene, which is the half `shot` never exercises.** A shot
    /// walks a staged cell into `Rtx::sWorld`; this walks one assembled figure into a view scene of
    /// its own, poses it, hands it over and traces it — the path the game takes for a doll and the
    /// race preview, and the one that had no test above the camera arithmetic.
    int runDoll(World& world, const ESM::NPC& npc, const PictureRequest& request);

    /// One local-map tile of a cell: an orthographic trace of the staged world, straight down.
    ///
    /// The game's own framing, from `MWRender::LocalMap` — one cell across, from fifty thousand
    /// units up, with a flat light that makes no shadows because a chart is read for what is where.
    int runMap(World& world, const ESM::Cell& cell, const StagingRequest& staging, const ActorRequest& actors,
        const PictureRequest& request);
}
