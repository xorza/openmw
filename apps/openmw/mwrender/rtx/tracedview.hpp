#pragma once

#include <cstddef>

#include <osg/Matrixf>
#include <osg/Node>
#include <osg/ref_ptr>

#include <components/myguirtx/rendermanager.hpp>
#include <components/rtx/offscreentrace.hpp>
#include <components/rtx/walk.hpp>

#include "../offscreenview.hpp"

namespace osg
{
    class FrameStamp;
    class Image;
}

namespace Resource
{
    class ImageManager;
}

namespace MWRender
{
    class RtxRenderer;

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

    /// Which of the seam's two pictures a view is: `Rtx::ViewRequest::mSubject`'s own word, said as
    /// a word rather than as a pointer that repeats the spec's scene.
    enum class ViewKind
    {
        /// A tile of the world, traced against the scene the renderer holds.
        World,

        /// A subject the game assembled for the picture, mirrored into a scene of its own.
        Subject,
    };

    /// An offscreen view as a ray tracer makes one: the GUI's side of `Rtx::OffscreenTrace`.
    ///
    /// **What is here is what the trace is not.** The picture itself — the camera, the subject's own
    /// mirrored scene, the hand-over and the trace — is `Rtx::OffscreenTrace`, which names no GUI
    /// and which the harness draws a doll with. This owns the `MyGUI::ITexture` the trace writes
    /// into, the copy in main memory the global map asks for, and the one piece of scheduling only
    /// the game has: a map tile asked for before there is a world to draw it against.
    ///
    /// **One class for both kinds of picture**, answering `SubjectView` and handed out as the base
    /// for a tile: the trace underneath is one object either way, and what a subject adds — an
    /// extent, a rebuild, a pick — the trace already answers for a subject and is never asked for
    /// a tile.
    class TracedView final : public SubjectView
    {
    public:
        /// **`traversals` is the one sequence every mirror walk here poses at** — the world's and
        /// every view's. A subtree both can reach would otherwise be posed by whichever counter
        /// got there first and frozen for the other.
        ///
        /// @param gui whose texture the trace writes into, and which draws it.
        TracedView(const OffscreenViewSpec& spec, ViewKind kind, RtxRenderer& host, MyGUIRtx::RenderManager& gui,
            Rtx::Traversals& traversals);
        ~TracedView() override;

        void setView(const osg::Matrixf& view) override { mTrace.setView(view); }
        void setExtent(int width, int height) override;
        void sceneChanged() override;
        void redraw() override;

        /// The drawing `redraw` asked for: the subject posed, walked and handed over, and the trace
        /// recorded. The host calls it inside the frame's window, once there is a world.
        void draw();

        bool isOfWorld() const { return mTrace.isOfWorld(); }

        void keepCopy() override;
        const osg::Image* getCopy() override;
        bool pick(float x, float y, osg::NodePath& hit) const override { return mTrace.pick(x, y, hit); }
        MyGUI::ITexture& getTexture() const override;

    private:
        /// Where the copy in main memory stands, for `getCopy`.
        ///
        /// **`OffscreenView::getCopy` promises null until the copy holds the picture the most
        /// recent `redraw()` asked for**, and a black image is not a picture that has not arrived —
        /// it is a picture of nothing. The global map paints the tile it is handed and marks the
        /// cell done, so answering early paints that cell black for the rest of the session. So
        /// between `redraw` and the `draw` the host answers it with, and until the trace that
        /// `draw` recorded has landed, the answer is null: what the backend holds until then is
        /// the trace before.
        enum class CopyState
        {
            /// Nobody asked `keepCopy`, so no trace leaves one.
            NotWanted,

            /// A redraw is asked for and not yet drawn.
            Queued,

            /// The trace that leaves the copy is recorded, and the copy not yet taken.
            Recorded,

            /// `mCopy` holds the picture.
            Taken,
        };

        RtxRenderer& mHost;
        Rtx::OffscreenTrace mTrace;

        /// The interface's own texture, taken from the manager under a name of this view's own:
        /// its slot in the renderer's table is what is traced into, and the handle is what gives
        /// both back.
        MyGUIRtx::TextureHandle mTexture;

        osg::ref_ptr<osg::Image> mCopy;
        CopyState mCopyState = CopyState::NotWanted;
    };

}
