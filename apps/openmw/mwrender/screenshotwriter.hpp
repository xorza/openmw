#ifndef GAME_RENDER_SCREENSHOTWRITER_H
#define GAME_RENDER_SCREENSHOTWRITER_H

#include <filesystem>

#include <osg/ref_ptr>

namespace SceneUtil
{
    class AsyncScreenCaptureOperation;
    class WorkQueue;
}

namespace MWRender
{
    /// The writer both renderers hand a captured frame to.
    ///
    /// **Shared rather than one apiece**, so the two write the same files to the same place with the
    /// same names and say the same thing afterwards. Where the picture came from — a frame buffer or
    /// a trace — is the renderer's business; the format, the path and the message are the game's.
    osg::ref_ptr<SceneUtil::AsyncScreenCaptureOperation> makeScreenshotWriter(
        SceneUtil::WorkQueue& queue, const std::filesystem::path& screenshotPath);
}

#endif
