#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include <osg/ref_ptr>

// The writer is held by reference count, so a `ref_ptr` member wants it complete.
#include <components/rtx/frameimage.hpp>
#include <components/sceneutil/screencapture.hpp>

namespace osg
{
    class Image;
}

namespace Rtx
{
    class Renderer;
}

namespace MWRender
{
    /// Everything that takes a finished frame off the device and puts it somewhere.
    ///
    /// **Not part of drawing one.** A savegame thumbnail, a screenshot and the frozen frame behind
    /// a loading screen all read the same picture back and differ only in what they do with it, so
    /// the read is here once and the frame path knows nothing about any of them.
    ///
    /// **The renderer is handed in rather than held**, because the window and the device are built
    /// after this and a frame is only ever read while both stand.
    class FrameCapture
    {
    public:
        explicit FrameCapture(osg::ref_ptr<SceneUtil::AsyncScreenCaptureOperation> writer)
            : mWriter(std::move(writer))
        {
        }

        /// The frame as it stands, borrowed until the next read.
        Rtx::TracedFrame read(Rtx::Renderer& renderer);

        /// A savegame thumbnail at the size the caller asked for, written into `image`.
        void thumbnail(Rtx::Renderer& renderer, osg::Image& image, int width, int height);

        /// A screenshot, through the writer the game gave this.
        void screenshot(Rtx::Renderer& renderer);

        /// Stops the writer, before the memory the frames it holds is given back.
        void stop();

    private:
        osg::ref_ptr<SceneUtil::AsyncScreenCaptureOperation> mWriter;

        /// The frame read back, refilled per read and never freed.
        std::vector<std::uint8_t> mPixels;
    };
}
