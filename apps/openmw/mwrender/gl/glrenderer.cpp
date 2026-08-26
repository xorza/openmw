#include "glrenderer.hpp"

#include <components/sceneutil/vismask.hpp>

#include <atomic>
#include <cmath>
#include <fstream>
#include <ostream>
#include <sstream>
#include <stdexcept>

#include <SDL.h>

#include <osg/Camera>
#include <osg/DisplaySettings>
#include <osg/GraphicsContext>
#include <osg/Image>
#include <osg/Stats>
#include <osg/Texture2D>

#include <osgDB/ReaderWriter>
#include <osgDB/Registry>

#include <osgGA/EventQueue>

#include <osgUtil/IncrementalCompileOperation>

#include <osgViewer/Renderer>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>

#include <components/debug/debuglog.hpp>
#include <components/debug/gldebug.hpp>
#include <components/l10n/manager.hpp>
#include <components/myguiplatform/myguiplatform.hpp>
#include <components/myguiplatform/myguirendermanager.hpp>
#include <components/myguiplatform/myguitexture.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/resource/stats.hpp>
#include <components/sceneutil/color.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/glextensions.hpp>
#include <components/sceneutil/screencapture.hpp>
#include <components/sceneutil/util.hpp>
#include <components/sceneutil/workqueue.hpp>
#include <components/sdlutil/imagetosurface.hpp>
#include <components/sdlutil/sdlgraphicswindow.hpp>
#include <components/settings/values.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/stereo/stereomanager.hpp>

#include "../../mwbase/environment.hpp"
#include "../../mwbase/windowmanager.hpp"

#include "../../profile.hpp"

#include "../renderingmanager.hpp"
#include "../sceneframe.hpp"
#include "../stage.hpp"
#include "../windowsetup.hpp"

#include "gloffscreenview.hpp"

#include "../screenshotwriter.hpp"
#include "postprocessor.hpp"

#include "screenshotmanager.hpp"

namespace
{
    void checkSDLError(int ret)
    {
        if (ret != 0)
            Log(Debug::Error) << "SDL error: " << SDL_GetError();
    }

    void initStatsHandler(Resource::Profiler& profiler)
    {
        const osg::Vec4f textColor(1.f, 1.f, 1.f, 1.f);
        const osg::Vec4f barColor(1.f, 1.f, 1.f, 1.f);
        const float multiplier = 1000;
        const bool average = true;
        const bool averageInInverseSpace = false;
        const float maxValue = 10000;

        OMW::forEachUserStatsValue([&](const OMW::UserStats& v) {
            profiler.addUserStatsLine(v.mLabel, textColor, barColor, v.mTaken, multiplier, average,
                averageInInverseSpace, v.mBegin, v.mEnd, maxValue);
        });
        // the forEachUserStatsValue loop is "run" at compile time, hence the settings manager is not available.
        // Unconditionnally add the async physics stats, and then remove it at runtime if necessary
        if (Settings::physics().mAsyncNumThreads == 0)
            profiler.removeUserStatsLine(" -Async");
    }

    class IdentifyOpenGLOperation : public osg::GraphicsOperation
    {
    public:
        IdentifyOpenGLOperation()
            : GraphicsOperation("IdentifyOpenGLOperation", false)
        {
        }

        void operator()(osg::GraphicsContext* graphicsContext) override
        {
            Log(Debug::Info) << "OpenGL Vendor: " << glGetString(GL_VENDOR);
            Log(Debug::Info) << "OpenGL Renderer: " << glGetString(GL_RENDERER);
            Log(Debug::Info) << "OpenGL Version: " << glGetString(GL_VERSION);
            glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &mMaxTextureImageUnits);
        }

        int getMaxTextureImageUnits() const
        {
            if (mMaxTextureImageUnits == 0)
                throw std::logic_error("mMaxTextureImageUnits is not initialized");
            return mMaxTextureImageUnits;
        }

    private:
        int mMaxTextureImageUnits = 0;
    };
}

namespace MWRender
{
    GlRenderer::GlRenderer(const RendererSpec& spec)
        : mStage(spec.mStage)
        , mSelectDepthFormatOperation(new SceneUtil::SelectDepthFormatOperation())
        , mSelectColorFormatOperation(new SceneUtil::Color::SelectColorFormatOperation())
    {
        mViewer = new osgViewer::Viewer;
        mViewer->getCamera()->getOrCreateStateSet()->removeAttribute(osg::StateAttribute::MATERIAL);
        SceneUtil::disableFFPStateForRenderer(
            static_cast<osgViewer::Renderer*>(mViewer->getCamera()->getRenderer()));
        mViewer->setReleaseContextAtEndOfFrameHint(false);
        mViewer->setLightingMode(osgViewer::View::NO_LIGHT);

        // Do not try to outsmart the OS thread scheduler (see bug #4785).
        mViewer->setUseConfigureAffinity(false);

        // Before the window, because `Stereo::getStereo()` is what decides whether the realize
        // operations get an initialiser and only the manager's constructor answers it.
        const bool stereoEnabled
            = Settings::stereo().mStereoEnabled || osg::DisplaySettings::instance().get()->getStereo();
        mStereoManager = std::make_unique<Stereo::Manager>(
            mViewer, stereoEnabled, Settings::camera().mNearClip, Settings::camera().mViewingDistance);

        // Taken from the viewer rather than made and handed to it: the viewer wires its update and
        // event visitors to the frame stamp at construction, and substituting objects underneath
        // without substituting those references is a bug that shows up frames later.
        mStage.adopt(
            *mViewer->getCamera(), *mViewer->getFrameStamp(), *mViewer->getEventQueue(), *mViewer->getViewerStats());

        createWindow(spec.mResourceDir);

        mScreenCaptureOperation = makeScreenshotWriter(spec.mWorkQueue, spec.mScreenshotPath);

        mScreenCaptureHandler = new osgViewer::ScreenCaptureHandler(mScreenCaptureOperation);
        mViewer->addEventHandler(mScreenCaptureHandler);

        mScreenshotManager = std::make_unique<ScreenshotManager>(*this, mStage);
    }

    GlRenderer::~GlRenderer()
    {
        if (mScreenCaptureOperation != nullptr)
            mScreenCaptureOperation->stop();

        mScreenshotManager.reset();
        mStereoManager.reset();
        mViewer = nullptr;

        // `SDL_GL_DeleteContext` on a window that has already gone is undefined, and the graphics
        // window would otherwise be torn down whenever the stage lets the camera go.
        if (mGraphicsWindow != nullptr)
            mGraphicsWindow->close();
        mGraphicsWindow = nullptr;

        if (mWindow != nullptr)
            SDL_DestroyWindow(mWindow);
    }

    void GlRenderer::createWindow(const std::filesystem::path& resourceDir)
    {
        const SDLUtil::VSyncMode vsync = Settings::video().mVsyncMode;
        unsigned antialiasing = static_cast<unsigned>(Settings::video().mAntialiasing);

        // Everything about where the window goes and what it is; the one flag naming what will be
        // drawn into it is this renderer's, and the GL attributes below it are too.
        const WindowPlacement placement = describeWindow(SDL_WINDOW_OPENGL);
        const int width = placement.mWidth;
        const int height = placement.mHeight;

        checkSDLError(SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8));
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8));
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8));
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0));
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24));
        if (Debug::shouldDebugOpenGL())
            checkSDLError(SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG));

        if (antialiasing > 0)
        {
            checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1));
            checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing));
        }

        osg::ref_ptr<SDLUtil::GraphicsWindowSDL2>& graphicsWindow = mGraphicsWindow;
        while (!graphicsWindow || !graphicsWindow->valid())
        {
            while (!mWindow)
            {
                mWindow = SDL_CreateWindow("OpenMW", placement.mX, placement.mY, width, height, placement.mFlags);
                if (!mWindow)
                {
                    // Try with a lower AA
                    if (antialiasing > 0)
                    {
                        Log(Debug::Warning) << "Warning: " << antialiasing << "x antialiasing not supported, trying "
                                            << antialiasing / 2;
                        antialiasing /= 2;
                        Settings::video().mAntialiasing.set(antialiasing);
                        checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing));
                        continue;
                    }
                    else
                    {
                        std::stringstream error;
                        error << "Failed to create SDL window: " << SDL_GetError();
                        throw std::runtime_error(error.str());
                    }
                }
            }

            // Since we use physical resolution internally, we have to create the window with scaled resolution,
            // but we can't get the scale before the window exists, so instead we have to resize aftewards.
            int w, h;
            SDL_GetWindowSize(mWindow, &w, &h);
            int dw, dh;
            SDL_GL_GetDrawableSize(mWindow, &dw, &dh);
            if (dw != w || dh != h)
            {
                SDL_SetWindowSize(mWindow, width / (dw / w), height / (dh / h));
            }

            MWRender::setWindowIcon(*mWindow, resourceDir);

            osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
            SDL_GetWindowPosition(mWindow, &traits->x, &traits->y);
            SDL_GL_GetDrawableSize(mWindow, &traits->width, &traits->height);
            traits->windowName = SDL_GetWindowTitle(mWindow);
            traits->windowDecoration = !(SDL_GetWindowFlags(mWindow) & SDL_WINDOW_BORDERLESS);
            traits->screenNum = SDL_GetWindowDisplayIndex(mWindow);
            traits->vsync = 0;
            traits->inheritedWindowData = new SDLUtil::GraphicsWindowSDL2::WindowData(mWindow);

            graphicsWindow = new SDLUtil::GraphicsWindowSDL2(traits, vsync);
            if (!graphicsWindow->valid())
                throw std::runtime_error("Failed to create GraphicsContext");

            if (traits->samples < antialiasing)
            {
                Log(Debug::Warning) << "Warning: Framebuffer MSAA level is only " << traits->samples << "x instead of "
                                    << antialiasing << "x. Trying " << antialiasing / 2 << "x instead.";
                graphicsWindow->closeImplementation();
                SDL_DestroyWindow(mWindow);
                mWindow = nullptr;
                antialiasing /= 2;
                Settings::video().mAntialiasing.set(antialiasing);
                checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing));
                continue;
            }

            if (traits->red < 8)
                Log(Debug::Warning) << "Warning: Framebuffer only has a " << traits->red << " bit red channel.";
            if (traits->green < 8)
                Log(Debug::Warning) << "Warning: Framebuffer only has a " << traits->green << " bit green channel.";
            if (traits->blue < 8)
                Log(Debug::Warning) << "Warning: Framebuffer only has a " << traits->blue << " bit blue channel.";
            if (traits->depth < 24)
                Log(Debug::Warning) << "Warning: Framebuffer only has " << traits->depth << " bits of depth precision.";

            traits->alpha = 0; // set to 0 to stop ScreenCaptureHandler reading the alpha channel
        }

        osg::Camera& camera = mStage.getCamera();
        camera.setGraphicsContext(graphicsWindow);
        camera.setViewport(0, 0, graphicsWindow->getTraits()->width, graphicsWindow->getTraits()->height);

        osg::ref_ptr<SceneUtil::OperationSequence> realizeOperations = new SceneUtil::OperationSequence(false);
        mViewer->setRealizeOperation(realizeOperations);
        osg::ref_ptr<IdentifyOpenGLOperation> identifyOp = new IdentifyOpenGLOperation();
        realizeOperations->add(identifyOp);
        realizeOperations->add(new SceneUtil::GetGLExtensionsOperation());

        if (Debug::shouldDebugOpenGL())
            realizeOperations->add(new Debug::EnableGLDebugOperation());

        realizeOperations->add(mSelectDepthFormatOperation);
        realizeOperations->add(mSelectColorFormatOperation);

        if (Stereo::getStereo())
        {
            Stereo::Settings settings;

            settings.mMultiview = Settings::stereo().mMultiview;
            settings.mAllowDisplayListsForMultiview = Settings::stereo().mAllowDisplayListsForMultiview;
            settings.mSharedShadowMaps = Settings::stereo().mSharedShadowMaps;

            if (Settings::stereo().mUseCustomView)
            {
                const osg::Vec3 leftEyeOffset(Settings::stereoView().mLeftEyeOffsetX,
                    Settings::stereoView().mLeftEyeOffsetY, Settings::stereoView().mLeftEyeOffsetZ);

                const osg::Quat leftEyeOrientation(Settings::stereoView().mLeftEyeOrientationX,
                    Settings::stereoView().mLeftEyeOrientationY, Settings::stereoView().mLeftEyeOrientationZ,
                    Settings::stereoView().mLeftEyeOrientationW);

                const osg::Vec3 rightEyeOffset(Settings::stereoView().mRightEyeOffsetX,
                    Settings::stereoView().mRightEyeOffsetY, Settings::stereoView().mRightEyeOffsetZ);

                const osg::Quat rightEyeOrientation(Settings::stereoView().mRightEyeOrientationX,
                    Settings::stereoView().mRightEyeOrientationY, Settings::stereoView().mRightEyeOrientationZ,
                    Settings::stereoView().mRightEyeOrientationW);

                settings.mCustomView = Stereo::CustomView{
                    .mLeft = Stereo::View{
                        .pose = Stereo::Pose{
                            .position = leftEyeOffset,
                            .orientation = leftEyeOrientation,
                        },
                        .fov = Stereo::FieldOfView{
                            .angleLeft = Settings::stereoView().mLeftEyeFovLeft,
                            .angleRight = Settings::stereoView().mLeftEyeFovRight,
                            .angleUp = Settings::stereoView().mLeftEyeFovUp,
                            .angleDown = Settings::stereoView().mLeftEyeFovDown,
                        },
                    },
                    .mRight = Stereo::View{
                        .pose = Stereo::Pose{
                            .position = rightEyeOffset,
                            .orientation = rightEyeOrientation,
                        },
                        .fov = Stereo::FieldOfView{
                            .angleLeft = Settings::stereoView().mRightEyeFovLeft,
                            .angleRight = Settings::stereoView().mRightEyeFovRight,
                            .angleUp = Settings::stereoView().mRightEyeFovUp,
                            .angleDown = Settings::stereoView().mRightEyeFovDown,
                        },
                    },
                };
            }

            if (Settings::stereo().mUseCustomEyeResolution)
                settings.mEyeResolution
                    = osg::Vec2i(Settings::stereoView().mEyeResolutionX, Settings::stereoView().mEyeResolutionY);

            realizeOperations->add(new Stereo::InitializeStereoOperation(settings));
        }

        mViewer->realize();
        mMaxTextureUnits = identifyOp->getMaxTextureImageUnits();

#if OSG_VERSION_LESS_THAN(3, 6, 6)
        // hack fix for https://github.com/openscenegraph/OpenSceneGraph/issues/1028
        osg::GLExtensions& exts = SceneUtil::getGLExtensions();
        if (!osg::isGLExtensionSupported(exts.contextID, "NV_framebuffer_multisample_coverage"))
            exts.glRenderbufferStorageMultisampleCoverageNV = nullptr;
#endif

        mStage.getEvents().getCurrentEventState()->setWindowRectangle(
            0, 0, graphicsWindow->getTraits()->width, graphicsWindow->getTraits()->height);
    }
    bool GlRenderer::wantsPagedTerrain() const
    {
        return Settings::terrain().mDistantTerrain;
    }

    float GlRenderer::getTerrainCompositeMapLevel() const
    {
        return static_cast<float>(std::pow(2, Settings::terrain().mCompositeMapLevel.get()));
    }

    float GlRenderer::getTerrainViewDistance(const float cameraDistance, const float fov) const
    {
        // Since our fog is not radial yet, we should take FOV in account, otherwise terrain near viewing distance may
        // disappear. Limit FOV here just for sure, otherwise viewing distance can be too high.
        const float distanceMult = std::cos(osg::DegreesToRadians(std::min(fov, 140.f)) / 2.f);
        return cameraDistance * (distanceMult ? 1.f / distanceMult : 1.f);
    }

    void GlRenderer::attachWorld(RenderingManager& world, osg::Group& worldRoot)
    {
        mResources = world.getResourceSystem();

        // **The chain goes above the world and becomes what is traversed.** Its constructor reads
        // `GLExtensions` off the camera's graphics context, which is why no renderer without one can
        // have it and why nothing above this line decides whether to build it.
        mPostProcessor = new PostProcessor(world, *this, mStage, &worldRoot, mResources->getVFS());
        setSceneRoot(*mPostProcessor);

        Resource::SceneManager& scene = *world.getResourceSystem()->getSceneManager();
        scene.setOpaqueDepthTex(mPostProcessor->getTexture(PostProcessor::Tex_OpaqueDepth, 0),
            mPostProcessor->getTexture(PostProcessor::Tex_OpaqueDepth, 1));
        scene.setOpaqueColorTex(mPostProcessor->getTexture(PostProcessor::Tex_OpaqueColor, 0),
            mPostProcessor->getTexture(PostProcessor::Tex_OpaqueColor, 1));
        scene.setSupportsNormalsRT(mPostProcessor->getSupportsNormalsRT());
    }

    void GlRenderer::setSceneRoot(osg::Group& root)
    {
        mStage.setSceneRoot(root);
        mViewer->setSceneData(&root);
    }

    void GlRenderer::showWorld(bool shown)
    {
        // The cull mask doubles as the record of which way round this is: nothing else leaves it at
        // exactly the two bits the interface is drawn with.
        constexpr unsigned int hidden = SceneUtil::Mask_GUI | SceneUtil::Mask_PreCompile;

        const auto cull = [this](unsigned int mask) {
            // The stereo pair goes with the one whatever the mode: both are no-ops in mono.
            mViewer->getCamera()->setCullMask(mask);
            mViewer->getCamera()->setCullMaskLeft(mask);
            mViewer->getCamera()->setCullMaskRight(mask);
        };

        if (!shown && mViewer->getCamera()->getCullMask() != hidden)
        {
            mShownUpdateMask = mViewer->getUpdateVisitor()->getTraversalMask();
            mShownCullMask = mViewer->getCamera()->getCullMask();
            mViewer->getUpdateVisitor()->setTraversalMask(hidden);
            cull(hidden);
        }
        else if (shown && mViewer->getCamera()->getCullMask() == hidden)
        {
            mViewer->getUpdateVisitor()->setTraversalMask(mShownUpdateMask);
            cull(mShownCullMask);
        }
    }

    void GlRenderer::advance(double simulationTime)
    {
        mViewer->advance(simulationTime);
    }

    void GlRenderer::eventTraversal()
    {
        mViewer->eventTraversal();
    }

    void GlRenderer::updateTraversal()
    {
        mViewer->updateTraversal();
    }

    // The world is already in the graph and the cull is what finds it, so the rasterizer has
    // nothing to read off the frame it is handed, and nothing to leave out when there is none. Both
    // of these are one traversal, and which of the two it was is a question only a renderer that
    // mirrors the graph has to answer.
    void GlRenderer::renderFrame(const SceneFrame& frame)
    {
        retireFreezeFrame();

        mPostProcessor->describe(frame.mWorld);

        mViewer->renderingTraversals();
    }

    /// Copies the framebuffer into a texture. Installed as an *initial* draw callback, so what it
    /// copies is the frame before the one about to be drawn — which is the one the player was
    /// looking at when the load began.
    class CopyFramebufferToTextureCallback : public osg::Camera::DrawCallback
    {
    public:
        explicit CopyFramebufferToTextureCallback(osg::Texture2D* texture)
            : mTexture(texture)
        {
        }

        void operator()(osg::RenderInfo& renderInfo) const override
        {
            const osg::Viewport* viewPort = renderInfo.getCurrentCamera()->getViewport();
            const int w = static_cast<int>(viewPort->width());
            const int h = static_cast<int>(viewPort->height());
            mTexture->copyTexImage2D(*renderInfo.getState(), 0, 0, w, h);

            mCopied.store(true, std::memory_order_release);
        }

        /// Whether it has run, read from the frame loop so that the callback can be taken out again.
        /// A full-screen copy is not something to keep paying for once the frame is frozen.
        bool copied() const { return mCopied.load(std::memory_order_acquire); }

        void arm() { mCopied.store(false, std::memory_order_release); }

    private:
        osg::ref_ptr<osg::Texture2D> mTexture;
        mutable std::atomic<bool> mCopied{ false };
    };

    MyGUI::ITexture& GlRenderer::freezeFrame()
    {
        if (!mFrozenFrame)
        {
            mFrozenFrame = new osg::Texture2D;
            mFrozenFrame->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            mFrozenFrame->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
            mFrozenFrame->setInternalFormat(GL_RGB);
            mFrozenFrame->setResizeNonPowerOfTwoHint(false);

            mFreezeFrame = new CopyFramebufferToTextureCallback(mFrozenFrame);
            mFrozenFrameTexture = std::make_unique<MyGUIPlatform::OSGTexture>(mFrozenFrame);
        }

        // Removed and added rather than left in place: the callback is what makes the copy happen,
        // and a loading screen wants the frame that was on the screen when it started, not the one
        // that was there when the last load did.
        mFreezeFrame->arm();
        mStage.getCamera().removeInitialDrawCallback(mFreezeFrame);
        mStage.getCamera().addInitialDrawCallback(mFreezeFrame);
        mFreezing = true;

        return *mFrozenFrameTexture;
    }

    void GlRenderer::retireFreezeFrame()
    {
        if (!mFreezing || !mFreezeFrame->copied())
            return;

        mStage.getCamera().removeInitialDrawCallback(mFreezeFrame);
        mFreezing = false;
    }

    std::unique_ptr<OffscreenView> GlRenderer::createOffscreenView(const OffscreenViewSpec& spec)
    {
        // Above the post-processing chain rather than inside it: a pre-render camera has to be
        // reached before the frame it feeds, and what it draws is not part of that frame.
        return std::make_unique<GlOffscreenView>(spec, mStage.getSceneRoot(), *mResources);
    }

    void GlRenderer::renderGui()
    {
        retireFreezeFrame();

        mViewer->renderingTraversals();
    }

    bool GlRenderer::done() const
    {
        return mViewer->done();
    }

    void GlRenderer::capture(osg::Image& image, int width, int height)
    {
        mScreenshotManager->screenshot(&image, width, height);
    }

    void GlRenderer::saveScreenshot()
    {
        mScreenCaptureHandler->setFramesToCapture(1);
        mScreenCaptureHandler->captureNextFrame(*mViewer);
    }

    void GlRenderer::suspendDraw()
    {
        mViewer->stopThreading();
    }

    void GlRenderer::resumeDraw()
    {
        mViewer->startThreading();
    }

    osgUtil::IncrementalCompileOperation* GlRenderer::getCompileOperation() const
    {
        return mViewer->getIncrementalCompileOperation();
    }

    void GlRenderer::setCompileOperation(osgUtil::IncrementalCompileOperation* operation)
    {
        mViewer->setIncrementalCompileOperation(operation);
    }

    void GlRenderer::setVSync(SDLUtil::VSyncMode mode)
    {
        osgViewer::Viewer::Windows windows;
        mViewer->getWindows(windows);
        mViewer->stopThreading();
        for (osgViewer::GraphicsWindow* window : windows)
        {
            if (auto* sdl2Window = dynamic_cast<SDLUtil::GraphicsWindowSDL2*>(window))
                sdl2Window->setSyncToVBlank(mode);
            else
                window->setSyncToVBlank(mode != SDLUtil::VSyncMode::Disabled);
        }
        mViewer->startThreading();
    }

    void GlRenderer::reloadChangedShaders(Shader::ShaderManager& shaders)
    {
        shaders.update(*mViewer);
    }

    std::unique_ptr<MyGUIPlatform::Platform> GlRenderer::createGuiPlatform(osg::Group& guiRoot,
        Resource::ImageManager& images, Shader::ShaderManager& shaders, const VFS::Manager& vfs, float scalingFactor,
        VFS::Path::NormalizedView resourcePath, const std::filesystem::path& logPath)
    {
        auto manager
            = std::make_unique<MyGUIPlatform::RenderManager>(mStage.getCamera(), &guiRoot, &images, scalingFactor);
        MyGUIPlatform::RenderManager& gui = *manager;

        auto platform = std::make_unique<MyGUIPlatform::Platform>(std::move(manager), &vfs, resourcePath, logPath);

        // **Which program the GUI is drawn with is this renderer's business**, and it is settled
        // after the platform rather than before it: the drawable the program goes on is made by the
        // `initialise` the platform's constructor calls.
        gui.enableShaders(shaders);

        return platform;
    }

    osg::Timer_t GlRenderer::getStartTick() const
    {
        return mViewer->getStartTick();
    }

    void GlRenderer::installStatsOverlay(const VFS::Manager& vfs, bool toFile)
    {
        osg::ref_ptr<Resource::Profiler> profiler = new Resource::Profiler(toFile, vfs);
        initStatsHandler(*profiler);
        mViewer->addEventHandler(profiler);

        mViewer->addEventHandler(new Resource::StatsHandler(toFile, vfs));

        if (toFile)
            Resource::collectStatistics(*mViewer);
    }

    void GlRenderer::reportStats(unsigned frameNumber, std::ostream& stream) const
    {
        mViewer->getViewerStats()->report(stream, frameNumber);
        osgViewer::Viewer::Cameras cameras;
        mViewer->getCameras(cameras);
        for (osg::Camera* camera : cameras)
            camera->getStats()->report(stream, frameNumber);
    }
}
