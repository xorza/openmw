#ifndef MWRENDER_SCREENSHOTMANAGER_H
#define MWRENDER_SCREENSHOTMANAGER_H

#include <osg/ref_ptr>

namespace osg
{
    class Image;
}

namespace MWRender
{
    class NotifyDrawCompletedCallback;
    class Renderer;
    class Stage;

    /// The frame the screenshot key and the save thumbnails get: one drawn on demand, out of band,
    /// read back off the frame buffer once the draw thread says it is there.
    class ScreenshotManager
    {
    public:
        ScreenshotManager(Renderer& renderer, Stage& stage);
        ~ScreenshotManager();

        void screenshot(osg::Image* image, int w, int h);

    private:
        Renderer& mRenderer;
        Stage& mStage;
        osg::ref_ptr<NotifyDrawCompletedCallback> mDrawCompleteCallback;
    };
}

#endif
