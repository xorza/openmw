#pragma once

#include <cstddef>
#include <optional>

namespace osg
{
    class FrameStamp;
}

namespace Resource
{
    class ImageManager;
}

namespace Rtx
{
    class Renderer;
}

namespace MWRender
{
    class TracedView;

    /// The moment a picture of its own subject walks that subject at.
    ///
    /// **Asked for per redraw and never stored**, because two of the three change every frame. A
    /// view that kept them would pose against whichever frame it was made on.
    struct PoseMoment
    {
        /// The renderer's own clock, which advances once per drawn frame whether or not the world's
        /// does. A doll posed against a stopped clock is a doll frozen the first time it was drawn.
        const osg::FrameStamp& mStamp;

        /// The game's frame number, which is which of a `SceneUtil::LightSource`'s two buffers
        /// update has just written. Not a pose number; see `Rtx::Traversals`.
        std::size_t mFrame = 0;

        Resource::ImageManager& mImages;
    };

    /// The few live links a traced view keeps to the renderer that made it.
    ///
    /// **An interface and not the renderer**, because a view is the one thing that needs a link
    /// back — it is drawn on a frame later than the one that asked for it — and these five calls
    /// are the whole of what it needs.
    class ViewHost
    {
    public:
        virtual ~ViewHost() = default;

        ViewHost(const ViewHost&) = delete;
        ViewHost& operator=(const ViewHost&) = delete;

        /// The backend a view traces into, and reads a picture back out of.
        virtual Rtx::Renderer& getBackend() = 0;

        /// Whether the world has reached the backend yet, so a picture traced against it would be a
        /// picture of something.
        virtual bool hasScene() const = 0;

        /// Nothing before the resource system has arrived, which is a view that cannot walk yet.
        virtual std::optional<PoseMoment> describePose() = 0;

        /// Draws `view` on the next frame that has a world in it.
        virtual void deferRedraw(TracedView& view) = 0;

        /// Takes a view off that list, because it is going away.
        virtual void forgetView(TracedView& view) = 0;

    protected:
        ViewHost() = default;
    };
}
