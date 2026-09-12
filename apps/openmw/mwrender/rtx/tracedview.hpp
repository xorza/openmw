#pragma once

#include <cstddef>
#include <cstdint>

#include <osg/Matrixf>
#include <osg/Node>
#include <osg/ref_ptr>

#include <components/rtx/offscreentrace.hpp>

#include "../offscreenview.hpp"

namespace MyGUI
{
    class ITexture;
}

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

    /// An offscreen view as a ray tracer makes one: the GUI's side of `Rtx::OffscreenTrace`.
    ///
    /// **What is here is what the trace is not.** The picture itself — the camera, the subject's own
    /// mirrored scene, the hand-over and the trace — is `Rtx::OffscreenTrace`, which names no GUI
    /// and which the harness draws a doll with. This owns the `MyGUI::ITexture` the trace writes
    /// into, the copy in main memory the global map asks for, and the one piece of scheduling only
    /// the game has: a map tile asked for before there is a world to draw it against.
    class TracedView final : public OffscreenView
    {
    public:
        /// **`traversals` is the one sequence every mirror walk here poses at** — the world's and
        /// every view's. A subtree both can reach would otherwise be posed by whichever counter
        /// got there first and frozen for the other.
        TracedView(const OffscreenViewSpec& spec, RtxRenderer& host, Rtx::Traversals& traversals);
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
        MyGUI::ITexture& getTexture() const override { return *mTexture; }

    private:
        RtxRenderer& mHost;
        Rtx::OffscreenTrace mTrace;

        /// Made through MyGUI's own factory, so which backend is behind it is not this class's
        /// business — but its slot in the renderer's table is, because that is what is traced into.
        MyGUI::ITexture* mTexture = nullptr;
        Rtx::GuiSlot mSlot;

        int mWidth = 0;
        int mHeight = 0;

        osg::ref_ptr<osg::Image> mCopy;

        /// Whether `mCopy` holds the picture the most recent `redraw()` asked for.
        ///
        /// **Because `OffscreenView::getCopy` promises null until it does**, and a black image is
        /// not a picture that has not arrived — it is a picture of nothing. The global map paints
        /// the tile it is handed and marks the cell done, so answering early paints that cell black
        /// for the rest of the session.
        bool mCopyIsCurrent = false;

        /// Between `redraw` and the `draw` the host answers it with. `getCopy` is null throughout,
        /// because what the backend holds until then is the trace before.
        bool mRedrawPending = false;

        bool mKeepCopy = false;
    };

}
