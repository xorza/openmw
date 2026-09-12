#include "framecapture.hpp"

#include <osg/Image>

#include <components/debug/debuglog.hpp>
#include <components/rtx/frameimage.hpp>
#include <components/rtx/renderer.hpp>

namespace MWRender
{
    Rtx::TracedFrame FrameCapture::read(Rtx::Renderer& renderer)
    {
        const Rtx::FrameExtents extents = renderer.getExtents();
        if (extents.mOutputWidth == 0 || extents.mOutputHeight == 0)
            return {};

        renderer.readPixels(mPixels);

        return Rtx::TracedFrame{
            .mWidth = extents.mOutputWidth,
            .mHeight = extents.mOutputHeight,
            .mPixels = mPixels,
        };
    }

    void FrameCapture::thumbnail(Rtx::Renderer& renderer, osg::Image& image, int width, int height)
    {
        // An out-parameter because the caller owns the image, so the shared conversion's result is
        // swapped into it rather than handed back. Three channels, for the reason `Rtx::Channels`
        // gives.
        const osg::ref_ptr<osg::Image> taken
            = Rtx::frameImage(read(renderer), width, height, Rtx::RowOrder::BottomFirst, Rtx::Channels::Rgb);
        if (taken == nullptr)
            return;

        image.swap(*taken);
    }

    void FrameCapture::screenshot(Rtx::Renderer& renderer)
    {
        const Rtx::TracedFrame frame = read(renderer);

        // Bottom row first, because what writes the file is `osgDB` through the same operation the
        // rasterizer hands `osgViewer`'s captures to, and that is the convention it reads.
        const osg::ref_ptr<osg::Image> taken = Rtx::frameImage(
            frame, static_cast<int>(frame.mWidth), static_cast<int>(frame.mHeight), Rtx::RowOrder::BottomFirst);

        if (taken == nullptr)
        {
            Log(Debug::Warning) << "Ray tracing has no frame to write a screenshot from";
            return;
        }

        // Straight to the writer rather than through a capture handler: the handler's job is to get
        // a frame off the graphics context, and this frame is already off it.
        (*mWriter)(*taken, 0);
    }

    void FrameCapture::stop()
    {
        if (mWriter != nullptr)
            mWriter->stop();
    }
}
