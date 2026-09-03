#include "framecapture.hpp"

#include <cstring>
#include <format>

#include <osg/Image>

#include <components/debug/debuglog.hpp>
#include <components/rtx/frameimage.hpp>
#include <components/rtx/png.hpp>
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
        // moved into it rather than handed back.
        const osg::ref_ptr<osg::Image> taken
            = Rtx::frameImage(read(renderer), width, height, Rtx::RowOrder::BottomFirst);
        if (taken == nullptr)
            return;

        // **Three channels and not four.** The one caller writes a savegame thumbnail as a JPEG,
        // which has no alpha to carry and whose writer refuses a four-channel image outright — an
        // `ERROR_IN_WRITING_FILE` and a save with no picture in it, which is what the rasterizer
        // avoids by reading its own screenshots back as `GL_RGB`.
        image.allocateImage(width, height, 1, GL_RGB, GL_UNSIGNED_BYTE);

        // Row by row, because three bytes a pixel is not a multiple of the packing: a thumbnail 518
        // across is 1,554 bytes of picture in a 1,556-byte row.
        for (int y = 0; y < height; ++y)
        {
            const std::uint8_t* from = taken->data(0, y);
            std::uint8_t* to = image.data(0, y);
            for (int x = 0; x < width; ++x)
                std::memcpy(to + x * 3, from + x * 4, 3);
        }
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

    void FrameCapture::keep(Rtx::Renderer& renderer)
    {
        if (mKeepLeft == 0)
            return;

        --mKeepLeft;

        const Rtx::FrameExtents extents = renderer.getExtents();
        renderer.readPixels(mPixels);

        const std::filesystem::path file = mKeepAt.string() + std::format("-{:04}.png", sKeepAtMost - mKeepLeft - 1);

        try
        {
            Rtx::writePng(file, extents.mOutputWidth, extents.mOutputHeight, mPixels);
        }
        catch (const std::exception& failed)
        {
            mKeepLeft = 0;
            Log(Debug::Error) << "Ray tracing could not write " << file << ": " << failed.what();
        }
    }

    void FrameCapture::keepFrames(const std::filesystem::path& where)
    {
        mKeepAt = where;
        mKeepLeft = sKeepAtMost;
        Log(Debug::Info) << "Ray tracing will write its first " << mKeepLeft << " frames to " << mKeepAt
                         << "-0000.png and on";
    }

    void FrameCapture::stop()
    {
        if (mWriter != nullptr)
            mWriter->stop();
    }
}
