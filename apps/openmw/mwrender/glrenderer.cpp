#include "glrenderer.hpp"

#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include <SDL.h>

#include <osg/BoundingSphere>
#include <osg/Camera>
#include <osg/DisplaySettings>
#include <osg/GraphicsContext>
#include <osg/Node>
#include <osg/Stats>
#include <osg/Texture2D>
#include <osg/Version>

#include <osgGA/EventQueue>

#include <osgUtil/IncrementalCompileOperation>

#include <osgViewer/Renderer>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>

#include <components/debug/debuglog.hpp>
#include <components/debug/gldebug.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/myguiplatform/myguiplatform.hpp>
#include <components/myguiplatform/myguirendermanager.hpp>
#include <components/myguiplatform/myguitexture.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/resource/stats.hpp>
#include <components/sceneutil/color.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/glextensions.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/screencapture.hpp>
#include <components/sceneutil/util.hpp>
#include <components/sceneutil/workqueue.hpp>
#include <components/sdlutil/sdlgraphicswindow.hpp>
#include <components/settings/values.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/stereo/stereomanager.hpp>
#include <components/terrain/quadtreeworld.hpp>
#include <components/terrain/terraingrid.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../profile.hpp"
#include "gloffscreenview.hpp"
#include "glworld.hpp"
#include "groundcover.hpp"
#include "objectpaging.hpp"
#include "postprocessor.hpp"
#include "renderingmanager.hpp"
#include "sceneframe.hpp"
#include "screenshotmanager.hpp"
#include "util.hpp"
#include "vismask.hpp"

namespace
{
    /// What the cull mask is left at while a screen covers the world.
    ///
    /// **It doubles as the record of which way round `showWorld` is**: nothing else leaves the mask
    /// at exactly the two bits the interface is drawn with.
    constexpr unsigned int sCoveredCullMask = MWRender::Mask_GUI | MWRender::Mask_PreCompile;

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

    // Upstream's, from loadingscreen.cpp.
    class DontComputeBoundCallback : public osg::Node::ComputeBoundingSphereCallback
    {
    public:
        osg::BoundingSphere computeBound(const osg::Node&) const override { return osg::BoundingSphere(); }
    };

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
        : mSelectDepthFormatOperation(new SceneUtil::SelectDepthFormatOperation())
        , mSelectColorFormatOperation(new SceneUtil::Color::SelectColorFormatOperation())
    {
        mViewer = new osgViewer::Viewer;
        mViewer->getCamera()->getOrCreateStateSet()->removeAttribute(osg::StateAttribute::MATERIAL);
        SceneUtil::disableFFPStateForRenderer(static_cast<osgViewer::Renderer*>(mViewer->getCamera()->getRenderer()));
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
        adopt(*mViewer->getCamera(), *mViewer->getFrameStamp(), *mViewer->getViewerStats());

        createWindow();

        compileIncrementally();

        mScreenshotManager = std::make_unique<ScreenshotManager>(mViewer);
    }

    GlRenderer::~GlRenderer()
    {
        mScreenshotManager.reset();
        mStereoManager.reset();
        mViewer = nullptr;

        // `SDL_GL_DeleteContext` on a window that has already gone is undefined, and the graphics
        // window would otherwise be torn down whenever the base lets the camera go.
        if (mGraphicsWindow != nullptr)
            mGraphicsWindow->close();
        mGraphicsWindow = nullptr;

        if (mWindow != nullptr)
            SDL_DestroyWindow(mWindow);
    }

    void GlRenderer::createWindow()
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

        osg::Camera& camera = getCamera();
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

        mViewer->getEventQueue()->getCurrentEventState()->setWindowRectangle(
            0, 0, graphicsWindow->getTraits()->width, graphicsWindow->getTraits()->height);
    }

    float GlRenderer::getGroundReach() const
    {
        // The setting rather than the world's live distance, which is the same number until a Lua
        // script sets its own: the map upstream draws is the size the setting says.
        return Settings::terrain().mDistantTerrain ? Settings::camera().mViewingDistance : 0.f;
    }

    // Upstream's, from RenderingManager's constructor: what the shader visitor is told before it
    // meets a model.
    void GlRenderer::prepareResources(Resource::ResourceSystem& resources)
    {
        mResources = &resources;

        Resource::SceneManager& scene = *resources.getSceneManager();
        scene.getShaderManager().setMaxTextureUnits(mMaxTextureUnits);

        scene.setAutoUseNormalMaps(Settings::shaders().mAutoUseObjectNormalMaps);
        scene.setNormalMapPattern(Settings::shaders().mNormalMapPattern);
        scene.setNormalHeightMapPattern(Settings::shaders().mNormalHeightMapPattern);
        scene.setAutoUseSpecularMaps(Settings::shaders().mAutoUseObjectSpecularMaps);
        scene.setSpecularMapPattern(Settings::shaders().mSpecularMapPattern);
        scene.setConvertAlphaTestToAlphaToCoverage(shouldAddMSAAIntermediateTarget());
        scene.setAdjustCoverageForAlphaTest(Settings::shaders().mAdjustCoverageForAlphaTest);
        scene.setWeatherParticleOcclusion(Settings::shaders().mWeatherParticleOcclusion);

        installStatsOverlay(*resources.getVFS());
    }

    // Upstream's, from RenderingManager's constructor: the light manager and what it is told
    // about the lighting method it settled on.
    osg::ref_ptr<osg::Group> GlRenderer::createSceneRoot()
    {
        // Let LightManager choose which backend to use based on our hint.
        // Ultimately dependent on support for various OpenGL extensions.
        osg::ref_ptr<SceneUtil::LightManager> sceneRoot = new SceneUtil::LightManager(
            SceneUtil::LightSettings{
                .mClusteredLighting = Settings::shaders().mClusteredLighting,
                .mMaxLights = Settings::shaders().mMaxLights,
                .mMaximumLightDistance = Settings::shaders().mMaximumLightDistance,
                .mLightFadeStart = Settings::shaders().mLightFadeStart,
                .mLightRadiusMultiplier = Settings::shaders().mLightRadiusMultiplier,
            },
            mResources);

        mResources->getSceneManager()->setSupportsClusteredLighting(sceneRoot->isClusteredSupported());

        // Sync clustered lighting setting so it's more intuitive when viewed in the in-game setting panel
        Settings::shaders().mClusteredLighting.set(sceneRoot->getClusteredLighting());

        sceneRoot->setLightingMask(Mask_Lighting);

        mSceneRoot = sceneRoot;
        return sceneRoot;
    }

    void GlRenderer::attachWorld(RenderingManager& world, osg::Group& worldRoot)
    {
        assert(mSceneRoot != nullptr && "the world is built under a root this renderer made");
        mWorld = std::make_unique<GlWorld>(*mViewer, world, worldRoot, *mSceneRoot, *mResources);

        // **The chain goes above the world and becomes what is traversed.**
        setTraversalRoot(mWorld->getPostProcessor());
    }

    void GlRenderer::detachWorld()
    {
        mWorld.reset();
        mSceneRoot = nullptr;
    }

    PostProcessor* GlRenderer::getPostProcessor()
    {
        return mWorld ? &mWorld->getPostProcessor() : nullptr;
    }

    void GlRenderer::adoptTraversalRoot(osg::Group& root)
    {
        mViewer->setSceneData(&root);
    }

    void GlRenderer::cull(unsigned int mask)
    {
        // The stereo pair goes with the one whatever the mode: both are no-ops in mono.
        mViewer->getCamera()->setCullMask(mask);
        mViewer->getCamera()->setCullMaskLeft(mask);
        mViewer->getCamera()->setCullMaskRight(mask);
    }

    void GlRenderer::showWorld(bool shown)
    {
        if (!shown && mViewer->getCamera()->getCullMask() != sCoveredCullMask)
        {
            mShownUpdateMask = mViewer->getUpdateVisitor()->getTraversalMask();
            mViewer->getUpdateVisitor()->setTraversalMask(sCoveredCullMask);
            cull(sCoveredCullMask);
        }
        else if (shown && mViewer->getCamera()->getCullMask() == sCoveredCullMask)
        {
            mViewer->getUpdateVisitor()->setTraversalMask(mShownUpdateMask);
            cull(getViewMask());
        }
    }

    void GlRenderer::applyViewMask(const unsigned int mask)
    {
        // **Not while a screen covers the world.** The camera then carries the two bits the
        // interface is drawn with, and `showWorld` writes the seam's word when the screen ends.
        if (mViewer->getCamera()->getCullMask() != sCoveredCullMask)
            cull(mask);
    }

    bool GlRenderer::toggleRenderMode(const RenderMode mode)
    {
        if (mode == Render_Wireframe)
            return mWorld->toggleWireframe();

        assert(mode == Render_Scene && "the other modes are the game's");

        unsigned int mask = getViewMask();

        const bool shown = (mask & sToggleWorldMask) == 0;
        if (shown)
            mask |= sToggleWorldMask;
        else
            mask &= ~sToggleWorldMask;

        setViewMask(mask);
        mWorld->setWorldShown(shown);
        return shown;
    }

    void GlRenderer::advance(double simulationTime)
    {
        mViewer->advance(simulationTime);

        // Every frame this renderer stamps, a loading screen's included, dumped once when its
        // figures have landed.
        const unsigned frameNumber = mViewer->getFrameStamp()->getFrameNumber();
        if (mStatsFile.is_open() && frameNumber >= sStatsReportDelay)
            reportStats(frameNumber - sStatsReportDelay);
    }

    void GlRenderer::eventTraversal()
    {
        mViewer->eventTraversal();
    }

    void GlRenderer::updateTraversal()
    {
        mViewer->updateTraversal();
    }

    void GlRenderer::describeFrame(const SceneFrame& frame)
    {
        // Whatever GLSL was edited since the last frame, recompiled before anything reads it. The
        // hot-reload manager stops the viewer's threads itself where it has to.
        mResources->getSceneManager()->getShaderManager().update(*mViewer);

        // The settings and not `frame.mEye`, which follows a Lua `setViewDistance`: what upstream fed
        // the stereo manager, exactly. Read by the stereo update callback, so before the traversal.
        mStereoManager->updateSettings(Settings::camera().mNearClip, Settings::camera().mViewingDistance);

        mWorld->describe(frame);
    }

    // The world is already in the graph and the cull is what finds it, so nothing is left out when
    // there is none: this and `renderGui` are one traversal, and which of the two it was is a
    // question only a renderer that mirrors the graph has to answer.
    void GlRenderer::renderFrame(const SceneFrame& frame)
    {
        mViewer->renderingTraversals();
    }

    class CopyFramebufferToTextureCallback : public osg::Camera::DrawCallback
    {
    public:
        CopyFramebufferToTextureCallback(osg::Texture2D* texture)
            : mOneshot(true)
            , mTexture(texture)
        {
        }

        void operator()(osg::RenderInfo& renderInfo) const override
        {
            const osg::Viewport* viewPort = renderInfo.getCurrentCamera()->getViewport();
            int w = static_cast<int>(viewPort->width());
            int h = static_cast<int>(viewPort->height());
            mTexture->copyTexImage2D(*renderInfo.getState(), 0, 0, w, h);

            mOneshot = false;
        }

        void reset() { mOneshot = true; }

    private:
        mutable bool mOneshot;
        osg::ref_ptr<osg::Texture2D> mTexture;
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

        getCamera().removeInitialDrawCallback(mFreezeFrame);
        getCamera().addInitialDrawCallback(mFreezeFrame);
        mFreezeFrame->reset();

        return *mFrozenFrameTexture;
    }

    // Upstream's, from RenderingManager::getWorldspaceChunkMgr.
    Ground GlRenderer::createGround(const GroundSpec& spec)
    {
        Ground ground;
        const ESM::RefId worldspace = spec.mWorldspace;
        osg::Group* sceneRoot = &spec.mSceneRoot;
        osg::Group* rootNode = &spec.mWorldRoot;
        Resource::ResourceSystem* resourceSystem = &spec.mResources;
        Terrain::Storage* storage = &spec.mStorage;

        const float lodFactor = Settings::terrain().mLodFactor;
        const bool groundcover = Settings::groundcover().mEnabled && worldspace == ESM::Cell::sDefaultWorldspaceId;
        const bool distantTerrain = Settings::terrain().mDistantTerrain;
        const double expiryDelay = Settings::cells().mCacheExpiryDelay;
        if (distantTerrain || groundcover)
        {
            const int compMapResolution = Settings::terrain().mCompositeMapResolution;
            const int compMapPower = Settings::terrain().mCompositeMapLevel;
            const float compMapLevel = static_cast<float>(std::pow(2, compMapPower));
            const int vertexLodMod = Settings::terrain().mVertexLodMod;
            const float maxCompGeometrySize = Settings::terrain().mMaxCompositeGeometrySize;
            const bool debugChunks = Settings::terrain().mDebugChunks;
            auto quadTreeWorld = std::make_unique<Terrain::QuadTreeWorld>(sceneRoot, rootNode, resourceSystem, storage,
                Mask_Terrain, Mask_PreCompile, Mask_Debug, compMapResolution, compMapLevel, lodFactor, vertexLodMod,
                maxCompGeometrySize, debugChunks, worldspace, expiryDelay);
            if (Settings::terrain().mObjectPaging)
            {
                ground.mObjectPaging = std::make_unique<ObjectPaging>(resourceSystem->getSceneManager(), worldspace);
                quadTreeWorld->addChunkManager(ground.mObjectPaging.get());
                resourceSystem->addResourceManager(ground.mObjectPaging.get());
            }
            if (groundcover)
            {
                const float groundcoverDistance = Settings::groundcover().mRenderingDistance;
                const float density = Settings::groundcover().mDensity;

                ground.mGroundcover = std::make_unique<Groundcover>(
                    resourceSystem->getSceneManager(), density, groundcoverDistance, spec.mGroundcoverStore);
                quadTreeWorld->addChunkManager(ground.mGroundcover.get());
                resourceSystem->addResourceManager(ground.mGroundcover.get());
            }
            ground.mTerrain = std::move(quadTreeWorld);
        }
        else
            ground.mTerrain = std::make_unique<Terrain::TerrainGrid>(sceneRoot, rootNode, resourceSystem, storage,
                Mask_Terrain, worldspace, expiryDelay, Mask_PreCompile, Mask_Debug);

        // The composite map's pace and the water's cull against the ground are this renderer's
        // chunks' to answer; the view distance is the game's and it sets that itself.
        ground.mTerrain->setTargetFrameRate(Settings::cells().mTargetFramerate);
        ground.mTerrain->enableHeightCullCallback(Settings::terrain().mWaterCulling);

        return ground;
    }

    // Both above the post-processing chain rather than inside it: a pre-render camera has to be
    // reached before the frame it feeds, and what it draws is not part of that frame.
    std::unique_ptr<OffscreenView> GlRenderer::createWorldView(const OffscreenViewSpec& spec)
    {
        return std::make_unique<GlTileView>(spec, getTraversalRoot(), getFrameStamp());
    }

    std::unique_ptr<SubjectView> GlRenderer::createSubjectView(const OffscreenViewSpec& spec)
    {
        return std::make_unique<GlDollView>(spec, getTraversalRoot(), getFrameStamp(), *mResources);
    }

    void GlRenderer::renderGui()
    {
        mViewer->renderingTraversals();
    }

    // Upstream's, from LoadingScreen::loadingOn, loadingOff and draw.
    void GlRenderer::beginLoading()
    {
        // Assign dummy bounding sphere callback to avoid the bounding sphere of the entire scene being recomputed after
        // each frame of loading We are already using node masks to avoid the scene from being updated/rendered, but
        // node masks don't work for computeBound()
        getTraversalRoot().setComputeBoundingSphereCallback(new DontComputeBoundCallback);

        if (const osgUtil::IncrementalCompileOperation* ico = mViewer->getIncrementalCompileOperation())
        {
            mLoadingIcoMin = ico->getMinimumTimeAvailableForGLCompileAndDeletePerFrame();
            mLoadingIcoMax = ico->getMaximumNumOfObjectsToCompilePerFrame();
        }
    }

    void GlRenderer::endLoading()
    {
        getTraversalRoot().setComputeBoundingSphereCallback(nullptr);
        getTraversalRoot().dirtyBound();

        if (osgUtil::IncrementalCompileOperation* ico = mViewer->getIncrementalCompileOperation())
        {
            ico->setMinimumTimeAvailableForGLCompileAndDeletePerFrame(mLoadingIcoMin);
            ico->setMaximumNumOfObjectsToCompilePerFrame(mLoadingIcoMax);
        }
    }

    void GlRenderer::applyLoadingBudget(const double targetFrameRate)
    {
        if (osgUtil::IncrementalCompileOperation* ico = mViewer->getIncrementalCompileOperation())
        {
            ico->setMinimumTimeAvailableForGLCompileAndDeletePerFrame(1.f / targetFrameRate);
            ico->setMaximumNumOfObjectsToCompilePerFrame(1000);
        }
    }

    void GlRenderer::capture(osg::Image& image, int width, int height)
    {
        mScreenshotManager->screenshot(&image, width, height);
    }

    void GlRenderer::setScreenshotWriter(SceneUtil::AsyncScreenCaptureOperation& writer)
    {
        Renderer::setScreenshotWriter(writer);

        mScreenCaptureHandler = new osgViewer::ScreenCaptureHandler(&writer);
        mViewer->addEventHandler(mScreenCaptureHandler);
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

    void GlRenderer::compileIncrementally()
    {
        if (getenv("OPENMW_DONT_PRECOMPILE") != nullptr)
            return;

        const osg::ref_ptr<osgUtil::IncrementalCompileOperation> ico = new osgUtil::IncrementalCompileOperation;
        ico->setTargetFrameRate(Settings::cells().mTargetFramerate);
        mViewer->setIncrementalCompileOperation(ico);
    }

    osgUtil::IncrementalCompileOperation* GlRenderer::getCompileOperation() const
    {
        return mViewer->getIncrementalCompileOperation();
    }

    void GlRenderer::processChangedSettings(const Settings::CategorySettingVector& changed)
    {
        if (mWorld)
            mWorld->processChangedSettings(changed);
    }

    void GlRenderer::addCell(const MWWorld::CellStore* cell)
    {
        mWorld->addCell(cell);
    }

    void GlRenderer::removeCell(const MWWorld::CellStore* cell)
    {
        mWorld->removeCell(cell);
    }

    void GlRenderer::addWaterRippleEmitter(const MWWorld::Ptr& ptr)
    {
        mWorld->addWaterRippleEmitter(ptr);
    }

    void GlRenderer::removeWaterRippleEmitter(const MWWorld::Ptr& ptr)
    {
        mWorld->removeWaterRippleEmitter(ptr);
    }

    void GlRenderer::emitWaterRipple(const osg::Vec3f& position)
    {
        mWorld->emitWaterRipple(position);
    }

    void GlRenderer::notifyWorldSpaceChanged()
    {
        mWorld->clearRipples();
    }

    void GlRenderer::listAssetsToPreload(
        std::vector<VFS::Path::Normalized>& models, std::vector<VFS::Path::Normalized>& textures)
    {
        mWorld->listAssetsToPreload(models, textures);
    }

    // Upstream's `SDLUtil::VideoWrapper::setSyncToVBlank`, with the viewer this renderer owns.
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

    std::unique_ptr<MyGUIPlatform::Platform> GlRenderer::createGuiPlatform(
        float scalingFactor, VFS::Path::NormalizedView resourcePath, const std::filesystem::path& logPath)
    {
        // Upstream's, from Engine::prepareEngine: the node MyGUI's camera hangs under, beside the
        // world under the root every traversal starts from, and masked so a covering screen can
        // cull the interface in and the world out.
        osg::ref_ptr<osg::Group> guiRoot = new osg::Group;
        guiRoot->setName("GUI Root");
        guiRoot->setNodeMask(Mask_GUI);
        getTraversalRoot().addChild(guiRoot);

        mStereoManager->disableStereoForNode(guiRoot);

        auto manager = std::make_unique<MyGUIPlatform::RenderManager>(
            mViewer, guiRoot, mResources->getImageManager(), scalingFactor);
        MyGUIPlatform::RenderManager& gui = *manager;

        auto platform = std::make_unique<MyGUIPlatform::Platform>(
            std::move(manager), mResources->getVFS(), resourcePath, logPath);

        // **Which program the GUI is drawn with is this renderer's business**, and it is settled
        // after the platform rather than before it: the drawable the program goes on is made by the
        // `initialise` the platform's constructor calls.
        gui.enableShaders(mResources->getSceneManager()->getShaderManager());

        return platform;
    }

    osg::Timer_t GlRenderer::getStartTick() const
    {
        return mViewer->getStartTick();
    }

    // Upstream's, from SDLUtil::InputWrapper.
    void GlRenderer::beginEvents()
    {
        mViewer->getEventQueue()->frame(0.f);
    }

    void GlRenderer::functionKey(const int index, const bool pressed)
    {
        const int key = osgGA::GUIEventAdapter::KEY_F1 + index;
        if (pressed)
            mViewer->getEventQueue()->keyPress(key);
        else
            mViewer->getEventQueue()->keyRelease(key);
    }

    void GlRenderer::windowResized(const int x, const int y, const int width, const int height)
    {
        mGraphicsWindow->resized(x, y, width, height);
        mViewer->getEventQueue()->windowResize(x, y, width, height);
    }

    // Upstream's, from Engine::go.
    void GlRenderer::installStatsOverlay(const VFS::Manager& vfs)
    {
#ifdef _WIN32
        const auto* statsFile = _wgetenv(L"OPENMW_OSG_STATS_FILE");
#else
        const auto* statsFile = std::getenv("OPENMW_OSG_STATS_FILE");
#endif

        std::filesystem::path path;
        if (statsFile != nullptr)
            path = statsFile;

        if (!path.empty())
        {
            mStatsFile.open(path, std::ios_base::out);
            if (mStatsFile.is_open())
                Log(Debug::Info) << "OSG stats will be written to: " << path;
            else
                Log(Debug::Warning) << "Failed to open file to write OSG stats \"" << path
                                    << "\": " << std::generic_category().message(errno);
        }

        const bool toFile = mStatsFile.is_open();

        osg::ref_ptr<Resource::Profiler> profiler = new Resource::Profiler(toFile, vfs);
        initStatsHandler(*profiler);
        mViewer->addEventHandler(profiler);

        mViewer->addEventHandler(new Resource::StatsHandler(toFile, vfs));

        if (toFile)
            Resource::collectStatistics(*mViewer);
    }

    void GlRenderer::reportStats(unsigned frameNumber)
    {
        mViewer->getViewerStats()->report(mStatsFile, frameNumber);
        osgViewer::Viewer::Cameras cameras;
        mViewer->getCameras(cameras);
        for (osg::Camera* camera : cameras)
            camera->getStats()->report(mStatsFile, frameNumber);
    }
}
