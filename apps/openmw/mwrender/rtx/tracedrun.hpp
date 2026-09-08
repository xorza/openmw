#pragma once

#include <cstdint>

namespace osg
{
    class Group;
}

namespace Resource
{
    class ResourceSystem;
}

namespace Rtx
{
    class Renderer;
    class SceneDesc;
    struct ExtractionStats;
}

namespace MWRender
{
    class Renderer;

    /// What a measured stop is allowed to look at, built once per stop by `RtxRenderer`.
    ///
    /// **Named, because `Session`, `StopWriter` and `checkHolds` each took the whole renderer.**
    /// Between them they read six things off it, and the twelve accessors that let them do it were
    /// the renderer's public surface — so the harness could reach anything the frame path owned and
    /// the call graph ran both ways through one class.
    ///
    /// Borrowed and valid for one stop: everything here is the renderer's own.
    struct TracedRun
    {
        /// The backend, for reading a picture or a channel back and for asking its extents.
        Rtx::Renderer& mBackend;

        /// The seam a picture inside the interface is made through — `createOffscreenView`, and the
        /// inventory doll's preview, which takes one of these of its own.
        Renderer& mViews;

        /// The scene the last walk handed over.
        const Rtx::SceneDesc& mScene;

        /// What this frame's walk found, and what a second walk over the same graph added.
        const Rtx::ExtractionStats& mWalked;
        const Rtx::ExtractionStats& mWalkedAgain;

        /// Where a picture of its own subject reads its textures from. Null before there is a world.
        Resource::ResourceSystem* mResources = nullptr;

        /// Whatever is topmost, for a picture of the world that has to be told what to draw.
        osg::Group* mSceneRoot = nullptr;

        /// How many textures the renderer has failed to read since it was built, and drew grey.
        std::uint32_t mUnreadableTextures = 0;
    };
}
