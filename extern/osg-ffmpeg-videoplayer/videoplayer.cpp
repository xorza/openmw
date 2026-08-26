#include "videoplayer.hpp"

#include <iostream>

#include <osg/Notify>
#include <osg/Image>

#include "audiofactory.hpp"
#include "videostate.hpp"

namespace Video
{

VideoPlayer::VideoPlayer()
    : mState(nullptr)
{

}

VideoPlayer::~VideoPlayer()
{
    if(mState)
        close();
}

void VideoPlayer::setAudioFactory(MovieAudioFactory *factory)
{
    mAudioFactory.reset(factory);
}

void VideoPlayer::playVideo(std::unique_ptr<std::istream>&& inputstream, const std::string& name)
{
    if(mState)
        close();

    try {
        mState = new VideoState;
        mState->setAudioFactory(mAudioFactory.get());
        mState->init(std::move(inputstream), name);

        // wait until we have the first picture
        while (mState->video_st && !mState->mImage.get())
        {
            if (!mState->update())
                break;
        }
    }
    catch(std::exception& e) {
        OSG_FATAL << "Failed to play video: " << e.what() << std::endl;
        close();
    }
}

bool VideoPlayer::update ()
{
    if(mState)
        return mState->update();
    return false;
}

void VideoPlayer::commitFrame()
{
    if (mState)
        mState->commitFrame();
}

const osg::Image* VideoPlayer::getVideoImage() const
{
    if (mState)
        return mState->mImage;
    return nullptr;
}

int VideoPlayer::getVideoWidth()
{
    int width=0;
    if (mState && mState->mImage.get())
        width = mState->mImage->s();
    return width;
}

int VideoPlayer::getVideoHeight()
{
    int height=0;
    if (mState && mState->mImage.get())
        height = mState->mImage->t();
    return height;
}

void VideoPlayer::close()
{
    if(mState)
    {
        mState->deinit();

        delete mState;
        mState = nullptr;
    }
}

bool VideoPlayer::hasAudioStream()
{
    return mState && mState->audio_st != nullptr;
}

void VideoPlayer::play()
{
    if (mState)
        mState->setPaused(false);
}

void VideoPlayer::pause()
{
    if (mState)
        mState->setPaused(true);
}

bool VideoPlayer::isPaused()
{
    if (mState)
        return mState->mPaused;
    return true;
}

double VideoPlayer::getCurrentTime()
{
    if (mState)
        return mState->get_master_clock();
    return 0.0;
}

void VideoPlayer::seek(double time)
{
    if (mState)
        mState->seekTo(time);
}

double VideoPlayer::getDuration()
{
    if (mState)
        return mState->getDuration();
    return 0.0;
}

}
