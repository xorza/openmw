#pragma once

#include <cstdint>
#include <optional>

#include <osg/Vec3f>

#include <components/rtx/extractionstats.hpp>
#include <components/rtx/framespend.hpp>
#include <components/rtx/reconstruction.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/shaders/visibility.h>

namespace Rtx
{
    class SceneDesc;
}

namespace Resource
{
    class ResourceSystem;
}

namespace MWRender
{
    class RtxRenderer;

    /// What a frame's walk found, and what a second walk over the same graph added where a run
    /// asked for one.
    ///
    /// **The second half is optional, because a walk that did not happen has no count.** Two
    /// members with the second default-constructed read as a walk that resolved nothing, which is
    /// the answer a check for "the second walk adds nothing" cannot tell from a pass.
    struct WalkReport
    {
        Rtx::ExtractionStats mFound;
        std::optional<Rtx::ExtractionStats> mAgain;
    };

    /// What one traced frame came to, handed to whoever measures it: what it spent, what the device
    /// answered for the frame behind, what put the picture back together, and what the walk found.
    struct FrameReport
    {
        /// What this fork owns of the frame, by phase. `Rtx::Timing` says which figure is a share
        /// of which.
        Rtx::FrameSpend mSpend;

        /// The whole frame, measured from one trace to the next: everything the game does between
        /// them, which is the number a player feels and the one `FrameResult::mWaitMs` cannot see.
        double mFrameMs = 0.0;

        /// Whether the hand-over rebuilt the scene from nothing, which a crossing is counted by.
        bool mRebuilt = false;

        /// How many meshes this frame's upload built structures for, `SceneUpload::mArrivedMeshes`.
        std::uint32_t mArrivedMeshes = 0;

        /// What the device answered for a frame behind, or nothing where it had finished none
        /// when this frame asked — the first frames of a run, and any frame the card was still
        /// busy for. Which frame it answers for is `FrameResult::mFrame`, never this one.
        std::optional<Rtx::FrameResult> mResult;

        /// What the backend numbered this frame — `Renderer::getFrameCount` as it stood before the
        /// frame was drawn — which is the number `mResult->mFrame` carries when this frame's own
        /// answer comes back, a frame or two from now.
        std::uint64_t mFrame = 0;

        /// What put this frame back together.
        Rtx::Reconstruction mReconstruction;

        /// What the frame was traced with beside the scene — the camera, the sky, the air, the sea
        /// and the sample — for the run's hashes to name when a picture moves and the scene did not.
        Rtx::Shaders::VisibilityConstants mConstants{};

        WalkReport mWalked;

        /// How many textures the renderer has failed to read since it was built, and drew grey.
        std::uint32_t mUnreadableTextures = 0;
    };

    /// What a measured stop may reach beyond the frame's own report. Borrowed and valid for one stop:
    /// everything here is the renderer's own.
    struct FrameContext
    {
        /// The renderer, which is the backend a stop reads through `getBackend`, the seam a picture
        /// inside the interface is made through, and what draws those pictures for a stop that
        /// wants one before the next frame.
        RtxRenderer& mRenderer;

        /// The world's. A pointer because upstream's `CharacterPreview` takes one; never null,
        /// because a frame is reported only while there is a world.
        Resource::ResourceSystem* mResources = nullptr;

        /// The scene the last walk handed over.
        const Rtx::SceneDesc& mScene;

        /// How much world the mirror builds, in units, and the eye it builds it around: the disc
        /// a check counts the ground in is the one the last walk stood.
        float mReach = 0.0f;
        osg::Vec3f mEye;
    };
}
