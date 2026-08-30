#include "videowidget.hpp"

#include <osg-ffmpeg-videoplayer/videoplayer.hpp>

#include <MyGUI_RenderManager.h>

#include <osg/Image>
#include <osg/Texture2D>

#include <components/debug/debuglog.hpp>
#include <components/myguiplatform/guirendermanager.hpp>
#include <components/vfs/manager.hpp>

#include "../mwsound/movieaudiofactory.hpp"

namespace MWGui
{

    VideoWidget::VideoWidget()
        : mVFS(nullptr)
        , mPicture("video frame")
    {
        mPlayer = std::make_unique<Video::VideoPlayer>();
        setNeedKeyFocus(true);
    }

    VideoWidget::~VideoWidget() = default;

    void VideoWidget::setVFS(const VFS::Manager* vfs)
    {
        mVFS = vfs;
    }

    void VideoWidget::playVideo(const std::string& video)
    {
        mPlayer->setAudioFactory(new MWSound::MovieAudioFactory());

        Files::IStreamPtr videoStream;
        try
        {
            videoStream = mVFS->get(video);
        }
        catch (std::exception& e)
        {
            Log(Debug::Error) << "Failed to open video: " << e.what();
            return;
        }

        mPlayer->playVideo(std::move(videoStream), video);

        osg::ref_ptr<osg::Texture2D> texture = mPlayer->getVideoTexture();
        if (!texture)
            return;

        // **Drawn where it lies where the backend can draw it.** The decoder owns this texture and
        // swaps the image under it as each frame is settled, so a backend that reads it needs
        // telling once and nothing after that. One that cannot see it at all is handed the pixels,
        // here and again every frame.
        mShared
            = static_cast<MyGUIPlatform::GuiRenderManager&>(MyGUI::RenderManager::getInstance()).shareTexture(*texture);

        if (mShared)
            setRenderItemTexture(mShared.get());
        else
        {
            mPicture.set(*texture->getImage());
            setRenderItemTexture(mPicture.getTexture());
        }

        // Both the widget and the video frame are Y-down, so this UV is not inverted
        getSubWidgetMain()->_setUVSet(MyGUI::FloatRect(0.f, 0.f, 1.f, 1.f));
    }

    int VideoWidget::getVideoWidth()
    {
        return mPlayer->getVideoWidth();
    }

    int VideoWidget::getVideoHeight()
    {
        return mPlayer->getVideoHeight();
    }

    bool VideoWidget::update()
    {
        return mPlayer->update();
    }

    void VideoWidget::commitFrame()
    {
        mPlayer->commitFrame();

        if (mShared)
            return;

        // **A whole frame back up to the device, every frame.** MyGUI's texture interface has no
        // way to hand over pixels other than all of them, and no way to reuse the buffer it lends
        // out. A video is a few minutes of a game that is otherwise doing nothing, so this is the
        // right side of that trade — but it is the reason nothing else should be drawn this way.
        const osg::ref_ptr<osg::Texture2D> texture = mPlayer->getVideoTexture();
        if (texture && texture->getImage() != nullptr)
            mPicture.set(*texture->getImage());
    }

    void VideoWidget::stop()
    {
        mPlayer->close();

        // The decoder's texture goes with the decoder's state, and this wrapper points at it.
        setRenderItemTexture(nullptr);
        mShared.reset();
    }

    void VideoWidget::pause()
    {
        mPlayer->pause();
    }

    void VideoWidget::resume()
    {
        mPlayer->play();
    }

    bool VideoWidget::isPaused() const
    {
        return mPlayer->isPaused();
    }

    bool VideoWidget::hasAudioStream()
    {
        return mPlayer->hasAudioStream();
    }

    void VideoWidget::autoResize(bool stretch)
    {
        MyGUI::IntSize screenSize = MyGUI::RenderManager::getInstance().getViewSize();
        if (getParent())
            screenSize = getParent()->getSize();

        if (getVideoHeight() > 0 && !stretch)
        {
            double imageaspect = static_cast<double>(getVideoWidth()) / getVideoHeight();

            int leftPadding = std::max(0, static_cast<int>(screenSize.width - screenSize.height * imageaspect) / 2);
            int topPadding = std::max(0, static_cast<int>(screenSize.height - screenSize.width / imageaspect) / 2);

            setCoord(leftPadding, topPadding, screenSize.width - leftPadding * 2, screenSize.height - topPadding * 2);
        }
        else
            setCoord(0, 0, screenSize.width, screenSize.height);
    }

}
