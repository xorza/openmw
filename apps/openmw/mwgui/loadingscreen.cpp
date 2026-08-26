#include "loadingscreen.hpp"

#include <array>

#include <osg/Camera>
#include <osg/FrameStamp>
#include <osg/Group>
#include <osg/Stats>

#include <osgUtil/IncrementalCompileOperation>

#include <MyGUI_Gui.h>
#include <MyGUI_ScrollBar.h>
#include <MyGUI_TextBox.h>
#include <MyGUI_UString.h>

#include <components/debug/debuglog.hpp>
#include <components/misc/pathhelpers.hpp>
#include <components/misc/rng.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/settings/values.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/recursivedirectoryiterator.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/statemanager.hpp"
#include "../mwbase/windowmanager.hpp"

#include "../mwrender/renderer.hpp"
#include "../mwrender/stage.hpp"

#include "backgroundimage.hpp"

namespace MWGui
{

    LoadingScreen::LoadingScreen(
        Resource::ResourceSystem* resourceSystem, MWRender::Renderer& renderer, MWRender::Stage& stage)
        : WindowBase("openmw_loading_screen.layout")
        , mResourceSystem(resourceSystem)
        , mRenderer(renderer)
        , mStage(stage)
        , mTargetFrameRate(120.0)
        , mLastWallpaperChangeTime(0.0)
        , mLastRenderTime(0.0)
        , mLoadingOnTime(0.0)
        , mImportantLabel(false)
        , mNestedLoadingCount(0)
        , mProgress(0)
        , mShowWallpaper(true)
    {
        getWidget(mLoadingText, "LoadingText");
        getWidget(mProgressBar, "ProgressBar");
        getWidget(mLoadingBox, "LoadingBox");
        getWidget(mSceneImage, "Scene");
        getWidget(mSplashImage, "Splash");

        mProgressBar->setScrollViewPage(1);

        findSplashScreens();
    }

    LoadingScreen::~LoadingScreen() {}

    void LoadingScreen::findSplashScreens()
    {
        auto isSupportedExtension = [](const std::string_view& ext) {
            static const std::array<std::string, 7> supportedExtensions{ { "tga", "dds", "ktx", "png", "bmp", "jpeg",
                "jpg" } };
            return !ext.empty()
                && std::find(supportedExtensions.begin(), supportedExtensions.end(), ext) != supportedExtensions.end();
        };

        constexpr VFS::Path::NormalizedView splash("splash/");
        for (const auto& name : mResourceSystem->getVFS()->getRecursiveDirectoryIterator(splash))
        {
            if (isSupportedExtension(Misc::getFileExtension(name)))
                mSplashScreens.push_back(name);
        }
        if (mSplashScreens.empty())
            Log(Debug::Warning) << "Warning: no splash screens found!";
    }

    void LoadingScreen::setLabel(const std::string& label, bool important)
    {
        mImportantLabel = important;

        mLoadingText->setCaptionWithReplacing(label);
        int padding = mLoadingBox->getWidth() - mLoadingText->getWidth();
        MyGUI::IntSize size(mLoadingText->getTextSize().width + padding, mLoadingBox->getHeight());
        size.width = std::max(300, size.width);
        mLoadingBox->setSize(size);

        if (MWBase::Environment::get().getWindowManager()->getMessagesCount() > 0)
            mLoadingBox->setPosition(mMainWidget->getWidth() / 2 - mLoadingBox->getWidth() / 2,
                mMainWidget->getHeight() / 2 - mLoadingBox->getHeight() / 2);
        else
            mLoadingBox->setPosition(mMainWidget->getWidth() / 2 - mLoadingBox->getWidth() / 2,
                mMainWidget->getHeight() - mLoadingBox->getHeight() - 8);
    }

    void LoadingScreen::setVisible(bool visible)
    {
        WindowBase::setVisible(visible);
        mSplashImage->setVisible(visible);
        mSceneImage->setVisible(visible);
    }

    double LoadingScreen::getTargetFrameRate() const
    {
        double frameRateLimit = MWBase::Environment::get().getFrameRateLimit();
        if (frameRateLimit > 0)
            return std::min(frameRateLimit, mTargetFrameRate);
        else
            return mTargetFrameRate;
    }

    class DontComputeBoundCallback : public osg::Node::ComputeBoundingSphereCallback
    {
    public:
        osg::BoundingSphere computeBound(const osg::Node&) const override { return osg::BoundingSphere(); }
    };

    void LoadingScreen::loadingOn()
    {
        // Early-out if already on
        if (mNestedLoadingCount++ > 0 && mMainWidget->getVisible())
            return;

        mLoadingOnTime = mTimer.time_m();

        // Assign dummy bounding sphere callback to avoid the bounding sphere of the entire scene being recomputed after
        // each frame of loading We are already using node masks to avoid the scene from being updated/rendered, but
        // node masks don't work for computeBound()
        mStage.getSceneRoot().setComputeBoundingSphereCallback(new DontComputeBoundCallback);

        if (const osgUtil::IncrementalCompileOperation* ico = mRenderer.getCompileOperation())
        {
            mOldIcoMin = ico->getMinimumTimeAvailableForGLCompileAndDeletePerFrame();
            mOldIcoMax = ico->getMaximumNumOfObjectsToCompilePerFrame();
        }

        setVisible(true);

        mShowWallpaper = MWBase::Environment::get().getStateManager()->getState() == MWBase::StateManager::State_NoGame;

        if (mShowWallpaper)
        {
            changeWallpaper();
        }

        MWBase::Environment::get().getWindowManager()->pushGuiMode(mShowWallpaper ? GM_LoadingWallpaper : GM_Loading);
    }

    void LoadingScreen::loadingOff()
    {
        if (--mNestedLoadingCount > 0)
            return;

        if (mLastRenderTime < mLoadingOnTime)
        {
            // the loading was so fast that we didn't show loading screen at all
            // we may still want to show the label if the caller requested it
            if (mImportantLabel)
            {
                MWBase::Environment::get().getWindowManager()->messageBox(mLoadingText->getCaption());
                mImportantLabel = false;
            }
        }
        else
            mImportantLabel = false; // label was already shown on loading screen

        mStage.getSceneRoot().setComputeBoundingSphereCallback(nullptr);
        mStage.getSceneRoot().dirtyBound();

        setVisible(false);

        if (osgUtil::IncrementalCompileOperation* ico = mRenderer.getCompileOperation())
        {
            ico->setMinimumTimeAvailableForGLCompileAndDeletePerFrame(mOldIcoMin);
            ico->setMaximumNumOfObjectsToCompilePerFrame(mOldIcoMax);
        }

        MWBase::Environment::get().getWindowManager()->removeGuiMode(GM_Loading);
        MWBase::Environment::get().getWindowManager()->removeGuiMode(GM_LoadingWallpaper);
    }

    void LoadingScreen::changeWallpaper()
    {
        if (!mSplashScreens.empty())
        {
            std::string const& randomSplash = mSplashScreens.at(Misc::Rng::rollDice(mSplashScreens.size()));

            // TODO: add option (filename pattern?) to use image aspect ratio instead of 4:3
            // we can't do this by default, because the Morrowind splash screens are 1024x1024, but should be displayed
            // as 4:3
            mSplashImage->setVisible(true);
            mSplashImage->setBackgroundImage(randomSplash, true, Settings::gui().mStretchMenuBackground);
        }
        mSceneImage->setBackgroundImage({});
        mSceneImage->setVisible(false);
    }

    void LoadingScreen::setProgressRange(size_t range)
    {
        mProgressBar->setScrollRange(range + 1);
        mProgressBar->setScrollPosition(0);
        mProgressBar->setTrackSize(0);
        mProgress = 0;
    }

    void LoadingScreen::setProgress(size_t value)
    {
        // skip expensive update if there isn't enough visible progress
        if (mProgressBar->getWidth() <= 0
            || value - mProgress < mProgressBar->getScrollRange() / mProgressBar->getWidth())
            return;
        value = std::min(value, mProgressBar->getScrollRange() - 1);
        mProgress = value;
        mProgressBar->setScrollPosition(0);
        mProgressBar->setTrackSize(
            static_cast<int>(value / (float)(mProgressBar->getScrollRange()) * mProgressBar->getLineSize()));
        draw();
    }

    void LoadingScreen::increaseProgress(size_t increase)
    {
        mProgressBar->setScrollPosition(0);
        size_t value = mProgress + increase;
        value = std::min(value, mProgressBar->getScrollRange() - 1);
        mProgress = value;
        mProgressBar->setTrackSize(
            static_cast<int>(value / (float)(mProgressBar->getScrollRange()) * mProgressBar->getLineSize()));
        draw();
    }

    bool LoadingScreen::needToDrawLoadingScreen()
    {
        if (mTimer.time_m() <= mLastRenderTime + (1.0 / getTargetFrameRate()) * 1000.0)
            return false;

        // the minimal delay before a loading screen shows
        constexpr float initialDelay = 0.05f;

        bool alreadyShown = (mLastRenderTime > mLoadingOnTime);
        double diff = (mTimer.time_m() - mLoadingOnTime);

        if (!alreadyShown)
        {
            // bump the delay by the current progress - i.e. if during the initial delay the loading
            // has almost finished, no point showing the loading screen now
            diff -= mProgress / static_cast<float>(mProgressBar->getScrollRange()) * 100.f;
        }

        if (!mShowWallpaper && diff < initialDelay * 1000)
            return false;
        return true;
    }

    void LoadingScreen::showFrozenFrame()
    {
        // The frame the player was looking at, held up behind the loading screen. The alternative —
        // clearing to nothing and letting the world show through — shakes on every buffer swap.

        mSplashImage->setBackgroundImage({});
        mSplashImage->setVisible(false);

        mSceneImage->setRenderItemTexture(&mRenderer.freezeFrame());
        // The widget is Y-down, the frozen frame is Y-up, so this UV is inverted
        mSceneImage->getSubWidgetMain()->_setUVSet(MyGUI::FloatRect(0.f, 1.f, 1.f, 0.f));
        mSceneImage->setVisible(true);
    }

    void LoadingScreen::draw()
    {
        if (!needToDrawLoadingScreen())
            return;

        if (mShowWallpaper && mTimer.time_m() > mLastWallpaperChangeTime + 5000 * 1)
        {
            mLastWallpaperChangeTime = mTimer.time_m();
            changeWallpaper();
        }

        if (!mShowWallpaper && mLastRenderTime < mLoadingOnTime)
        {
            showFrozenFrame();
        }

        MWBase::Environment::get().getInputManager()->update(0, true, true);

        osg::Stats& stats = mStage.getStats();
        const unsigned frameNumber = mStage.getFrameStamp().getFrameNumber();

        stats.setAttribute(frameNumber, "Loading", 1);

        mResourceSystem->reportStats(frameNumber, &stats);
        if (osgUtil::IncrementalCompileOperation* ico = mRenderer.getCompileOperation())
        {
            ico->setMinimumTimeAvailableForGLCompileAndDeletePerFrame(1.f / getTargetFrameRate());
            ico->setMaximumNumOfObjectsToCompilePerFrame(1000);
        }

        // at the time this function is called we are in the middle of a frame,
        // so out of order calls are necessary to get a correct frameNumber for the next frame.
        // refer to the advance() and frame() order in Engine::go()
        mRenderer.eventTraversal();
        mRenderer.updateTraversal();
        mRenderer.renderGui();
        mRenderer.advance(mStage.getFrameStamp().getSimulationTime());

        mLastRenderTime = mTimer.time_m();
    }

}
