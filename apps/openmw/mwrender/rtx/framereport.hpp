#pragma once

#include <cstdint>
#include <optional>

#include <components/rtx/extractionstats.hpp>
#include <components/rtx/framespend.hpp>
#include <components/rtx/reconstruction.hpp>
#include <components/rtx/renderer.hpp>

namespace Rtx
{
    class SceneDesc;
}

namespace osg
{
    class Group;
}

namespace Resource
{
    class ResourceSystem;
}

namespace MWRender
{
    class Renderer;
    class ViewHost;

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

        /// What the device answered for the frame behind, or nothing on the first frames of a run,
        /// which have no frame behind them to answer for.
        std::optional<Rtx::FrameResult> mResult;

        /// What put this frame back together.
        Rtx::Reconstruction mReconstruction;

        WalkReport mWalked;

        /// How many textures the renderer has failed to read since it was built, and drew grey.
        std::uint32_t mUnreadableTextures = 0;
    };

    /// What a measured stop may reach beyond the frame's own report. Borrowed and valid for one stop:
    /// everything here is the renderer's own.
    struct FrameContext
    {
        Rtx::Renderer& mBackend;

        /// The seam a picture inside the interface is made through — `createOffscreenView`, and
        /// the inventory doll's preview, which takes one of these of its own.
        Renderer& mViews;

        /// What draws those pictures, for a stop that wants one before the next frame.
        ViewHost& mHost;

        /// The world's, or null before there is a world.
        Resource::ResourceSystem* mResources = nullptr;

        /// Whatever is topmost, or null.
        osg::Group* mSceneRoot = nullptr;

        /// The scene the last walk handed over.
        const Rtx::SceneDesc& mScene;

        /// How much world the mirror builds, in units.
        float mReach = 0.0f;
    };
}
