#pragma once

#include <cstdint>
#include <vector>

#include <osg/Matrixf>
#include <osg/Node>
#include <osg/ref_ptr>

#include <components/rtx/offscreentrace.hpp>

#include "../offscreenview.hpp"
#include "viewhost.hpp"

namespace MyGUI
{
    class ITexture;
}

namespace osg
{
    class Image;
}

namespace MWRender
{
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
        TracedView(const OffscreenViewSpec& spec, ViewHost& host, Rtx::Traversals& traversals);
        ~TracedView() override;

        void setView(const osg::Matrixf& view) override { mTrace.setView(view); }
        void setExtent(int width, int height) override;
        void sceneChanged() override;
        void redraw() override;
        void keepCopy() override;
        const osg::Image* getCopy() const override;
        bool pick(float x, float y, osg::NodePath& hit) const override { return mTrace.pick(x, y, hit); }
        MyGUI::ITexture& getTexture() const override { return *mTexture; }

    private:
        ViewHost& mHost;
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

        /// What a read lands in before it is handed to the copy. Kept rather than made per redraw:
        /// the local map draws a tile a cell, and a cell arriving is already the busiest frame there
        /// is.
        std::vector<std::uint8_t> mPixels;

        bool mKeepCopy = false;
    };

}
