#include "renderingmanager.hpp"

#include <cstdlib>

#include <osg/Camera>
#include <osg/ClipControl>
#include <osg/ComputeBoundsVisitor>
#include <osg/FrameStamp>
#include <osg/Group>
#include <osg/Matrix>
#include <osg/Stats>
#include <osg/UserDataContainer>

#include <osgUtil/IncrementalCompileOperation>
#include <osgUtil/LineSegmentIntersector>

#include <components/nifosg/nifloader.hpp>

#include <components/debug/debuglog.hpp>

#include <components/stereo/multiview.hpp>
#include <components/stereo/stereomanager.hpp>

#include <components/resource/imagemanager.hpp>
#include <components/resource/keyframemanager.hpp>
#include <components/resource/resourcesystem.hpp>

#include <components/fallback/fallback.hpp>
#include <components/settings/values.hpp>

#include <components/sceneutil/cullsafeboundsvisitor.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/material.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/sceneutil/rtt.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/sceneutil/stateupdater.hpp>
#include <components/sceneutil/texmat.hpp>
#include <components/sceneutil/visitor.hpp>
#include <components/sceneutil/workqueue.hpp>
#include <components/sceneutil/writescene.hpp>

#include <components/misc/constants.hpp>

#include <components/terrain/quadtreeworld.hpp>
#include <components/terrain/terraingrid.hpp>

#include <components/esm3/loadcell.hpp>
#include <components/esm4/loadcell.hpp>

#include <components/debug/debugdraw.hpp>
#include <components/detournavigator/navigator.hpp>
#include <components/detournavigator/navmeshcacheitem.hpp>

#include "../mwworld/cellstore.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/groundcoverstore.hpp"
#include "../mwworld/scene.hpp"

#include "../mwgui/postprocessorhud.hpp"

#include "../mwmechanics/actorutil.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "actorspaths.hpp"
#include "camera.hpp"
#include "effectmanager.hpp"
#include "fogmanager.hpp"
#include "groundcover.hpp"
#include "navmesh.hpp"
#include "npcanimation.hpp"
#include "objectpaging.hpp"
#include "pathgrid.hpp"
#include "postprocessor.hpp"
#include "precipitation.hpp"
#include "recastmesh.hpp"
#include "renderer.hpp"
#include "sceneframe.hpp"
#include "terrainstorage.hpp"
#include "util.hpp"
#include "vismask.hpp"
#include "water.hpp"

namespace MWRender
{
    class PreloadCommonAssetsWorkItem : public SceneUtil::WorkItem
    {
    public:
        PreloadCommonAssetsWorkItem(Resource::ResourceSystem* resourceSystem)
            : mResourceSystem(resourceSystem)
        {
        }

        void doWork() override
        {
            try
            {
                for (const VFS::Path::Normalized& v : mModels)
                    mResourceSystem->getSceneManager()->getTemplate(v);
                for (const VFS::Path::Normalized& v : mTextures)
                    mResourceSystem->getImageManager()->getImage(v);
                for (const VFS::Path::Normalized& v : mKeyframes)
                    mResourceSystem->getKeyframeManager()->get(v);
            }
            catch (const std::exception& e)
            {
                Log(Debug::Warning) << "Failed to preload common assets: " << e.what();
            }
        }

        std::vector<VFS::Path::Normalized> mModels;
        std::vector<VFS::Path::Normalized> mTextures;
        std::vector<VFS::Path::Normalized> mKeyframes;

    private:
        Resource::ResourceSystem* mResourceSystem;
    };

    RenderingManager::RenderingManager(Renderer& renderer, osg::ref_ptr<osg::Group> rootNode,
        Resource::ResourceSystem* resourceSystem, SceneUtil::WorkQueue* workQueue,
        DetourNavigator::Navigator& navigator, const MWWorld::GroundcoverStore& groundcoverStore,
        SceneUtil::UnrefQueue& unrefQueue)
        : mRenderer(renderer)
        , mRootNode(rootNode)
        , mResourceSystem(resourceSystem)
        , mWorkQueue(workQueue)
        , mNavigator(navigator)
        , mTimescaleClouds(Fallback::Map::getBool("Weather_Timescale_Clouds"))
        , mNightEyeFactor(0.f)
        // TODO: Near clip should not need to be bounded like this, but too small values break OSG shadow calculations
        // CPU-side. See issue: #6072
        , mNearClip(Settings::camera().mNearClip)
        , mViewDistance(Settings::camera().mViewingDistance)
        , mFieldOfViewOverridden(false)
        , mFieldOfViewOverride(0.f)
        , mFieldOfView(Settings::camera().mFieldOfView)
        , mFirstPersonFieldOfView(Settings::camera().mFirstPersonFieldOfView)
        , mGroundCoverStore(groundcoverStore)
    {
        resourceSystem->getSceneManager()->setParticleSystemMask(MWRender::Mask_ParticleSystem);

        osg::ref_ptr<osg::Group> sceneRoot = mRenderer.createSceneRoot();
        mSceneRoot = sceneRoot;
        sceneRoot->setNodeMask(Mask_Scene);
        sceneRoot->setName("Scene Root");

        mNavMesh = std::make_unique<NavMesh>(mRootNode, mWorkQueue, Settings::navigator().mEnableNavMeshRender,
            Settings::navigator().mNavMeshRenderMode);
        mActorsPaths = std::make_unique<ActorsPaths>(mRootNode, Settings::navigator().mEnableAgentsPathsRender);
        mRecastMesh = std::make_unique<RecastMesh>(mRootNode, Settings::navigator().mEnableRecastMeshRender);
        mPathgrid = std::make_unique<Pathgrid>(mRootNode);

        mObjects = std::make_unique<Objects>(mResourceSystem, sceneRoot, unrefQueue);

        mResourceSystem->getSceneManager()->setIncrementalCompileOperation(mRenderer.getCompileOperation());

        mEffectManager = std::make_unique<EffectManager>(sceneRoot, mResourceSystem);

        const std::string& normalMapPattern = Settings::shaders().mNormalMapPattern;
        const std::string& heightMapPattern = Settings::shaders().mNormalHeightMapPattern;
        const std::string& specularMapPattern = Settings::shaders().mTerrainSpecularMapPattern;
        const bool useTerrainNormalMaps = Settings::shaders().mAutoUseTerrainNormalMaps;
        const bool useTerrainSpecularMaps = Settings::shaders().mAutoUseTerrainSpecularMaps;

        mTerrainStorage = std::make_unique<TerrainStorage>(mResourceSystem, normalMapPattern, heightMapPattern,
            useTerrainNormalMaps, specularMapPattern, useTerrainSpecularMaps);

        WorldspaceChunkMgr& chunkMgr = getWorldspaceChunkMgr(ESM::Cell::sDefaultWorldspaceId);
        mTerrain = chunkMgr.mTerrain.get();
        mGroundcover = chunkMgr.mGroundcover.get();
        mObjectPaging = chunkMgr.mObjectPaging.get();

        mSunLight = new SceneUtil::Light;
        mSunLight->setDiffuse(osg::Vec4f(0, 0, 0, 1));
        mSunLight->setAmbient(osg::Vec4f(0, 0, 0, 1));
        mSunLight->setSpecular(osg::Vec4f(0, 0, 0, 0));
        mSunLight->setConstantAttenuation(1.f);

        mPrecipitation
            = std::make_unique<Precipitation>(sceneRoot, &mRenderer.getCamera(), resourceSystem->getSceneManager());

        mRenderer.attachWorld(*this, *mRootNode);

        mCamera = std::make_unique<Camera>(&mRenderer.getCamera());

        osg::ref_ptr<SceneUtil::Material> defaultMat(new SceneUtil::Material);
        defaultMat->updateStateSet(sceneRoot->getOrCreateStateSet());
        sceneRoot->getOrCreateStateSet()->setAttribute(defaultMat);

        mFog = std::make_unique<FogManager>();

        auto mask = ~(Mask_UpdateVisitor | Mask_SimpleWater);
        MWBase::Environment::get().getWindowManager()->setCullMask(mask);
        NifOsg::Loader::setHiddenNodeMask(Mask_UpdateVisitor);
        NifOsg::Loader::setIntersectionDisabledNodeMask(Mask_Effect);
        NifOsg::Loader::setSoftEffectEnabled(Settings::shaders().mSoftParticles);

        updateProjectionMatrix();
    }

    RenderingManager::~RenderingManager()
    {
        mRenderer.detachWorld();

        // let background loading thread finish before we delete anything else
        mWorkQueue = nullptr;
    }

    osgUtil::IncrementalCompileOperation* RenderingManager::getIncrementalCompileOperation()
    {
        return mRenderer.getCompileOperation();
    }

    MWRender::Objects& RenderingManager::getObjects()
    {
        return *mObjects.get();
    }

    Resource::ResourceSystem* RenderingManager::getResourceSystem()
    {
        return mResourceSystem;
    }

    SceneUtil::WorkQueue* RenderingManager::getWorkQueue()
    {
        return mWorkQueue.get();
    }

    Terrain::World* RenderingManager::getTerrain()
    {
        return mTerrain;
    }

    void RenderingManager::preloadCommonAssets()
    {
        osg::ref_ptr<PreloadCommonAssetsWorkItem> workItem(new PreloadCommonAssetsWorkItem(mResourceSystem));
        mPrecipitation->listAssetsToPreload(workItem->mModels, workItem->mTextures);
        mRenderer.listAssetsToPreload(workItem->mModels, workItem->mTextures);

        workItem->mModels.push_back(Settings::models().mXbaseanim);
        workItem->mModels.push_back(Settings::models().mXbaseanim1st);
        workItem->mModels.push_back(Settings::models().mXbaseanimfemale);
        workItem->mModels.push_back(Settings::models().mXargonianswimkna);

        workItem->mKeyframes.push_back(Settings::models().mXbaseanimkf);
        workItem->mKeyframes.push_back(Settings::models().mXbaseanim1stkf);
        workItem->mKeyframes.push_back(Settings::models().mXbaseanimfemalekf);
        workItem->mKeyframes.push_back(Settings::models().mXargonianswimknakf);

        workItem->mTextures.emplace_back("textures/_land_default.dds");

        mWorkQueue->addWorkItem(std::move(workItem));
    }

    double RenderingManager::getReferenceTime() const
    {
        return mRenderer.getFrameStamp().getReferenceTime();
    }

    osg::Group* RenderingManager::getSceneRoot()
    {
        return mSceneRoot.get();
    }

    osg::Group& RenderingManager::getPrecipitationRoot()
    {
        return mPrecipitation->getRoot();
    }

    void RenderingManager::setNightEyeFactor(float factor)
    {
        if (factor != mNightEyeFactor)
        {
            mNightEyeFactor = factor;
            updateAmbient();
        }
    }

    void RenderingManager::setAmbientColour(const osg::Vec4f& colour)
    {
        mAmbientColor = colour;
        updateAmbient();
    }

    int RenderingManager::skyGetMasserPhase() const
    {
        return Sky::MoonState::phaseToInt(mSky.mMoons[0].mPhase);
    }

    int RenderingManager::skyGetSecundaPhase() const
    {
        return Sky::MoonState::phaseToInt(mSky.mMoons[1].mPhase);
    }

    void RenderingManager::skySetMoonColour(bool red)
    {
        mSky.mMoonRed = red;
    }

    void RenderingManager::setStormParticleDirection(const osg::Vec3f& direction)
    {
        mPrecipitation->setStormParticleDirection(direction);
    }

    void RenderingManager::setSunEnabled(bool enabled)
    {
        mSky.mSunEnabled = enabled;
    }

    void RenderingManager::setGlareFade(float fade)
    {
        mSky.mGlareFade = fade;
    }

    void RenderingManager::configureAmbient(const MWWorld::Cell& cell)
    {
        bool isInterior = !cell.isExterior() && !cell.isQuasiExterior();
        bool needsAdjusting = false;
        needsAdjusting = isInterior && (!Settings::shaders().mClassicFalloff || Settings::shaders().mClusteredLighting);

        osg::Vec4f ambient = SceneUtil::colourFromRGB(cell.getMood().mAmbiantColor);

        if (needsAdjusting)
        {
            constexpr float pR = 0.2126f;
            constexpr float pG = 0.7152f;
            constexpr float pB = 0.0722f;

            // we already work in linear RGB so no conversions are needed for the luminosity function
            float relativeLuminance = pR * ambient.r() + pG * ambient.g() + pB * ambient.b();
            const float minimumAmbientLuminance = Settings::shaders().mMinimumInteriorBrightness;
            if (relativeLuminance < minimumAmbientLuminance)
            {
                // brighten ambient so it reaches the minimum threshold but no more, we want to mess with content data
                // as least we can
                if (ambient.r() == 0.f && ambient.g() == 0.f && ambient.b() == 0.f)
                    ambient = osg::Vec4(
                        minimumAmbientLuminance, minimumAmbientLuminance, minimumAmbientLuminance, ambient.a());
                else
                    ambient *= minimumAmbientLuminance / relativeLuminance;
            }
        }

        setAmbientColour(ambient);

        osg::Vec4f diffuse = SceneUtil::colourFromRGB(cell.getMood().mDirectionalColor);

        setSunColour(diffuse, diffuse, 0.f);
        // This is total nonsense but it's what Morrowind uses
        static const osg::Vec4f interiorSunPos
            = osg::Vec4f(-1.f, osg::DegreesToRadians(45.f), osg::DegreesToRadians(45.f), 0.f);
        mSunLight->setPosition(interiorSunPos);

        // Where upstream pointed the chain's sun; the frame carries it instead
        mSky.mSunPosition = interiorSunPos;
        mSky.mSunVector = -interiorSunPos;
        mSky.mSunAtNight = false;
    }

    void RenderingManager::setSunColour(const osg::Vec4f& diffuse, const osg::Vec4f& specular, float sunVis)
    {
        // need to wrap this in a StateUpdater?
        mSunLight->setDiffuse(diffuse);
        mSunLight->setSpecular(osg::Vec4f(specular.x(), specular.y(), specular.z(), specular.w() * sunVis));

        mSky.mSunVisibility = sunVis;
    }

    const osg::Vec4f& RenderingManager::getSunLightPosition() const
    {
        return mSunLight->getPosition();
    }

    void RenderingManager::setSunDirection(const osg::Vec3f& direction)
    {
        osg::Vec3f position = -direction;

        // This is based on the exterior sun orbit and won't make sense for interiors, see WeatherManager::update
        position.z() = 400.f - std::abs(position.x());

        // The sun is not always synchronized with the sunlight because reasons
        const osg::Vec3f sunlightPos = Settings::shaders().mMatchSunlightToSun ? position : -direction;
        // need to wrap this in a StateUpdater?
        mSunLight->setPosition(osg::Vec4f(sunlightPos, 0.f));

        mSky.mSunPosition = osg::Vec4f(position, 0.f);
        mSky.mSunVector = osg::Vec4f(-sunlightPos, 0.f);
        mSky.mSunAtNight = mNight;
    }

    void RenderingManager::addCell(const MWWorld::CellStore* store)
    {
        mPathgrid->addCell(store);

        mRenderer.addCell(store);

        if (store->getCell()->isExterior())
        {
            enableTerrain(true, store->getCell()->getWorldSpace());
            mTerrain->loadCell(store->getCell()->getGridX(), store->getCell()->getGridY());
        }
    }
    void RenderingManager::removeCell(const MWWorld::CellStore* store)
    {
        mPathgrid->removeCell(store);
        mActorsPaths->removeCell(store);
        mObjects->removeCell(store);

        if (store->getCell()->isExterior())
        {
            getWorldspaceChunkMgr(store->getCell()->getWorldSpace())
                .mTerrain->unloadCell(store->getCell()->getGridX(), store->getCell()->getGridY());
        }

        mRenderer.removeCell(store);
    }

    void RenderingManager::enableTerrain(bool enable, ESM::RefId worldspace)
    {
        if (enable)
        {
            WorldspaceChunkMgr& newChunks = getWorldspaceChunkMgr(worldspace);
            if (newChunks.mTerrain.get() != mTerrain)
            {
                mTerrain->enable(false);
                mTerrain = newChunks.mTerrain.get();
                mGroundcover = newChunks.mGroundcover.get();
                mObjectPaging = newChunks.mObjectPaging.get();
            }
        }
        mTerrain->enable(enable);
    }

    void RenderingManager::setSkyEnabled(bool enabled)
    {
        mPrecipitation->setEnabled(enabled);
        mSky.mSkyEnabled = enabled;
    }

    bool RenderingManager::toggleBorders()
    {
        bool borders = !mTerrain->getBordersVisible();
        mTerrain->setBordersVisible(borders);
        return borders;
    }

    bool RenderingManager::toggleRenderMode(RenderMode mode)
    {
        if (mode == Render_CollisionDebug || mode == Render_Pathgrid)
            return mPathgrid->toggleRenderMode(mode);
        else if (mode == Render_Wireframe)
            return mRenderer.toggleRenderMode(mode);
        else if (mode == Render_Water)
            return mWaterToggled = !mWaterToggled;
        else if (mode == Render_Scene)
        {
            // Asked of the renderer, because a cull mask is only the rasterizer's way of saying it
            return mRenderer.toggleRenderMode(mode);
        }
        else if (mode == Render_NavMesh)
        {
            return mNavMesh->toggle();
        }
        else if (mode == Render_ActorsPaths)
        {
            return mActorsPaths->toggle();
        }
        else if (mode == Render_RecastMesh)
        {
            return mRecastMesh->toggle();
        }
        return false;
    }

    void RenderingManager::configureFog(const MWWorld::Cell& cell)
    {
        mFog->configure(mViewDistance, cell);
    }

    void RenderingManager::configureFog(
        float fogDepth, float underwaterFog, float dlFactor, float dlOffset, const osg::Vec4f& color)
    {
        mFog->configure(mViewDistance, fogDepth, underwaterFog, dlFactor, dlOffset, color);
    }

    void RenderingManager::update(float dt, bool paused)
    {
        reportStats();

        if (!paused)
        {
            mEffectManager->update(dt);
            mPrecipitation->update();
            updateSkyClocks(dt);
        }

        mFrameDelta = dt;
        mFramePaused = paused;

        updateNavMesh();
        updateRecastMesh();

        if (mUpdateProjectionMatrix)
        {
            mUpdateProjectionMatrix = false;
            updateProjectionMatrix();
        }
        mCamera->update(dt, paused);
    }

    void RenderingManager::updatePlayerPtr(const MWWorld::Ptr& ptr)
    {
        if (mPlayerAnimation.get())
        {
            setupPlayer(ptr);
            mPlayerAnimation->updatePtr(ptr);
        }
        mCamera->attachTo(ptr);
    }

    void RenderingManager::removePlayer(const MWWorld::Ptr& player)
    {
        mRenderer.removeWaterRippleEmitter(player);
    }

    void RenderingManager::rotateObject(const MWWorld::Ptr& ptr, const osg::Quat& rot)
    {
        if (ptr == mCamera->getTrackingPtr() && !mCamera->isVanityOrPreviewModeEnabled())
        {
            mCamera->rotateCameraToTrackingPtr();
        }

        ptr.getRefData().getBaseNode()->setAttitude(rot);
    }

    void RenderingManager::moveObject(const MWWorld::Ptr& ptr, const osg::Vec3f& pos)
    {
        ptr.getRefData().getBaseNode()->setPosition(pos);
    }

    void RenderingManager::scaleObject(const MWWorld::Ptr& ptr, const osg::Vec3f& scale)
    {
        ptr.getRefData().getBaseNode()->setScale(scale);

        if (ptr == mCamera->getTrackingPtr()) // update height of camera
            mCamera->processViewChange();
    }

    void RenderingManager::removeObject(const MWWorld::Ptr& ptr)
    {
        mActorsPaths->remove(ptr);
        mObjects->removeObject(ptr);
        mRenderer.removeWaterRippleEmitter(ptr);
    }

    void RenderingManager::setWaterEnabled(bool enabled)
    {
        mWaterEnabled = enabled;
        mPrecipitation->setWaterEnabled(enabled);
    }

    void RenderingManager::setWaterHeight(float height)
    {
        mWaterHeight = height;
        mPrecipitation->setWaterHeight(height);
    }

    bool RenderingManager::isUnderwater(const osg::Vec3f& position) const
    {
        return position.z() < mWaterHeight && mWaterToggled && mWaterEnabled;
    }

    void RenderingManager::screenshot(osg::Image* image, int w, int h)
    {
        mRenderer.capture(*image, w, h);
    }

    osg::Vec2f RenderingManager::getScreenCoords(const osg::BoundingBox& bb)
    {
        if (bb.valid())
        {
            const osg::Matrix viewProj
                = mRenderer.getCamera().getViewMatrix() * mRenderer.getCamera().getProjectionMatrix();
            const osg::Vec3f worldPoint((bb.xMin() + bb.xMax()) * 0.5f, (bb.yMin() + bb.yMax()) * 0.5f, bb.zMax());
            const osg::Vec4f clipPoint = osg::Vec4f(worldPoint, 1.0f) * viewProj;
            if (clipPoint.w() > 0.f)
            {
                const float screenPointX = (clipPoint.x() / clipPoint.w() + 1.f) * 0.5f;
                const float screenPointY = (clipPoint.y() / clipPoint.w() - 1.f) * (-0.5f);
                if (screenPointX >= 0.f && screenPointX <= 1.f && screenPointY >= 0.f && screenPointY <= 1.f)
                    return osg::Vec2f(screenPointX, screenPointY);
            }
        }

        return osg::Vec2f(0.5f, 0.f);
    }

    RenderingManager::RayResult getIntersectionResult(osgUtil::LineSegmentIntersector* intersector,
        const osg::ref_ptr<osgUtil::IntersectionVisitor>& visitor, std::span<const MWWorld::Ptr> ignoreList = {})
    {
        constexpr auto nonObjectWorldMask = Mask_Terrain | Mask_Water;
        RenderingManager::RayResult result;
        result.mHit = false;
        result.mRatio = 0;

        if (!intersector->containsIntersections())
            return result;

        auto test = [&](const osgUtil::LineSegmentIntersector::Intersection& intersection) {
            PtrHolder* ptrHolder = nullptr;
            std::vector<RefnumMarker*> refnumMarkers;
            bool hitNonObjectWorld = false;
            for (osg::Node* node : intersection.nodePath)
            {
                const auto& nodeMask = node->getNodeMask();
                if (!hitNonObjectWorld)
                    hitNonObjectWorld = nodeMask & nonObjectWorldMask;

                osg::UserDataContainer* userDataContainer = node->getUserDataContainer();
                if (!userDataContainer)
                    continue;
                for (unsigned int i = 0; i < userDataContainer->getNumUserObjects(); ++i)
                {
                    if (PtrHolder* p = dynamic_cast<PtrHolder*>(userDataContainer->getUserObject(i)))
                    {
                        if (std::find(ignoreList.begin(), ignoreList.end(), p->mPtr) == ignoreList.end())
                        {
                            ptrHolder = p;
                        }
                    }
                    if (RefnumMarker* r = dynamic_cast<RefnumMarker*>(userDataContainer->getUserObject(i)))
                    {
                        refnumMarkers.push_back(r);
                    }
                }
            }

            if (ptrHolder)
                result.mHitObject = ptrHolder->mPtr;

            unsigned int vertexCounter = 0;
            for (unsigned int i = 0; i < refnumMarkers.size(); ++i)
            {
                unsigned int intersectionIndex = intersection.indexList.empty() ? 0 : intersection.indexList[0];
                if (!refnumMarkers[i]->mNumVertices
                    || (intersectionIndex >= vertexCounter
                        && intersectionIndex < vertexCounter + refnumMarkers[i]->mNumVertices))
                {
                    auto it = std::find_if(
                        ignoreList.begin(), ignoreList.end(), [target = refnumMarkers[i]->mRefnum](const auto& ptr) {
                            return target == ptr.getCellRef().getRefNum();
                        });

                    if (it == ignoreList.end())
                    {
                        result.mHitRefnum = refnumMarkers[i]->mRefnum;
                    }

                    break;
                }
                vertexCounter += refnumMarkers[i]->mNumVertices;
            }

            if (!result.mHitObject.isEmpty() || result.mHitRefnum.isSet() || hitNonObjectWorld)
            {
                result.mHit = true;
                result.mHitPointWorld = intersection.getWorldIntersectPoint();
                result.mHitNormalWorld = intersection.getWorldIntersectNormal();
                result.mRatio = static_cast<float>(intersection.ratio);
            }
        };

        if (ignoreList.empty() || intersector->getIntersectionLimit() != osgUtil::LineSegmentIntersector::NO_LIMIT)
        {
            test(intersector->getFirstIntersection());
        }
        else
        {
            for (const auto& intersection : intersector->getIntersections())
            {
                test(intersection);

                if (result.mHit)
                {
                    break;
                }
            }
        }

        return result;
    }

    class IntersectionVisitorWithIgnoreList : public osgUtil::IntersectionVisitor
    {
    public:
        bool skipTransform(osg::Transform& transform)
        {
            if (mContainsPagedRefs)
                return false;

            osg::UserDataContainer* userDataContainer = transform.getUserDataContainer();
            if (!userDataContainer)
                return false;

            for (unsigned int i = 0; i < userDataContainer->getNumUserObjects(); ++i)
            {
                if (PtrHolder* p = dynamic_cast<PtrHolder*>(userDataContainer->getUserObject(i)))
                {
                    if (std::find(mIgnoreList.begin(), mIgnoreList.end(), p->mPtr) != mIgnoreList.end())
                    {
                        return true;
                    }
                }
            }

            return false;
        }

        void apply(osg::Transform& transform) override
        {
            if (skipTransform(transform))
            {
                return;
            }
            osgUtil::IntersectionVisitor::apply(transform);
        }

        void setIgnoreList(std::span<const MWWorld::Ptr> ignoreList) { mIgnoreList = ignoreList; }
        void setContainsPagedRefs(bool contains) { mContainsPagedRefs = contains; }

    private:
        std::span<const MWWorld::Ptr> mIgnoreList;
        bool mContainsPagedRefs = false;
    };

    osg::ref_ptr<osgUtil::IntersectionVisitor> RenderingManager::getIntersectionVisitor(
        osgUtil::Intersector* intersector, bool ignorePlayer, bool ignoreActors, bool ignoreTerrain,
        std::span<const MWWorld::Ptr> ignoreList)
    {
        if (!mIntersectionVisitor)
            mIntersectionVisitor = new IntersectionVisitorWithIgnoreList;

        mIntersectionVisitor->setIgnoreList(ignoreList);
        mIntersectionVisitor->setContainsPagedRefs(false);

        MWWorld::Scene* worldScene = MWBase::Environment::get().getWorldScene();
        for (const auto& ptr : ignoreList)
        {
            if (worldScene->isPagedRef(ptr))
            {
                mIntersectionVisitor->setContainsPagedRefs(true);
                intersector->setIntersectionLimit(osgUtil::LineSegmentIntersector::NO_LIMIT);
                break;
            }
        }

        mIntersectionVisitor->setTraversalNumber(mRenderer.getFrameStamp().getFrameNumber());
        mIntersectionVisitor->setFrameStamp(&mRenderer.getFrameStamp());
        mIntersectionVisitor->setIntersector(intersector);
        // Terrain picks its LOD from the eye point.
        mIntersectionVisitor->setReferenceEyePoint(osg::Vec3f());
        mIntersectionVisitor->setReferenceEyePointCoordinateFrame(osgUtil::Intersector::VIEW);

        unsigned int mask = ~0u;
        mask &= ~(Mask_RenderToTexture | Mask_Sky | Mask_Debug | Mask_Effect | Mask_Water | Mask_SimpleWater
            | Mask_Groundcover);
        if (ignorePlayer)
            mask &= ~(Mask_Player);
        if (ignoreActors)
            mask &= ~(Mask_Actor | Mask_Player);
        if (ignoreTerrain)
            mask &= ~(Mask_Terrain);

        mIntersectionVisitor->setTraversalMask(mask);
        return mIntersectionVisitor;
    }

    RenderingManager::RayResult RenderingManager::castRay(const osg::Vec3f& origin, const osg::Vec3f& dest,
        bool ignorePlayer, bool ignoreActors, bool ignoreTerrain, std::span<const MWWorld::Ptr> ignoreList)
    {
        osg::ref_ptr<osgUtil::LineSegmentIntersector> intersector(
            new osgUtil::LineSegmentIntersector(osgUtil::LineSegmentIntersector::MODEL, origin, dest));
        intersector->setIntersectionLimit(osgUtil::LineSegmentIntersector::LIMIT_NEAREST);

        osg::ref_ptr<osgUtil::IntersectionVisitor> visitor
            = getIntersectionVisitor(intersector, ignorePlayer, ignoreActors, ignoreTerrain, ignoreList);
        visitor->setReferenceEyePoint(origin);
        visitor->setReferenceEyePointCoordinateFrame(osgUtil::Intersector::MODEL);
        mRootNode->accept(*visitor);

        return getIntersectionResult(intersector, mIntersectionVisitor, ignoreList);
    }

    RenderingManager::RayResult RenderingManager::castCameraToViewportRay(
        const float nX, const float nY, float maxDistance, bool ignorePlayer, bool ignoreActors, bool ignoreTerrain)
    {
        osg::ref_ptr<osgUtil::LineSegmentIntersector> intersector(new osgUtil::LineSegmentIntersector(
            osgUtil::LineSegmentIntersector::PROJECTION, nX * 2.f - 1.f, nY * (-2.f) + 1.f));

        osg::Vec3d dist(0.f, 0.f, -maxDistance);

        dist = dist * mRenderer.getCamera().getProjectionMatrix();

        osg::Vec3d end = intersector->getEnd();
        end.z() = dist.z();
        intersector->setEnd(end);
        intersector->setIntersectionLimit(osgUtil::LineSegmentIntersector::LIMIT_NEAREST);

        mRenderer.getCamera().accept(*getIntersectionVisitor(intersector, ignorePlayer, ignoreActors, ignoreTerrain));

        return getIntersectionResult(intersector, mIntersectionVisitor);
    }

    void RenderingManager::updatePtr(const MWWorld::Ptr& old, const MWWorld::Ptr& updated)
    {
        mObjects->updatePtr(old, updated);
        mActorsPaths->updatePtr(old, updated);
    }

    void RenderingManager::spawnEffect(VFS::Path::NormalizedView model, std::string_view texture,
        const osg::Vec3f& worldPosition, float scale, bool isMagicVFX, bool useAmbientLight, std::string_view effectId,
        bool loop)
    {
        mEffectManager->addEffect(model, texture, worldPosition, scale, isMagicVFX, useAmbientLight, effectId, loop);
    }

    void RenderingManager::removeEffect(std::string_view effectId)
    {
        mEffectManager->removeEffect(effectId);
    }

    void RenderingManager::notifyWorldSpaceChanged()
    {
        mEffectManager->clear();
        mRenderer.notifyWorldSpaceChanged();
    }

    void RenderingManager::clear()
    {
        mSky.mMoonRed = false;

        notifyWorldSpaceChanged();
        mRenderer.forgetReferences();
        if (mObjectPaging)
            mObjectPaging->clear();
    }

    MWRender::Animation* RenderingManager::getAnimation(const MWWorld::Ptr& ptr)
    {
        if (mPlayerAnimation.get() && ptr == mPlayerAnimation->getPtr())
            return mPlayerAnimation.get();

        return mObjects->getAnimation(ptr);
    }

    const MWRender::Animation* RenderingManager::getAnimation(const MWWorld::ConstPtr& ptr) const
    {
        if (mPlayerAnimation.get() && ptr == mPlayerAnimation->getPtr())
            return mPlayerAnimation.get();

        return mObjects->getAnimation(ptr);
    }

    PostProcessor* RenderingManager::getPostProcessor()
    {
        return mRenderer.getPostProcessor();
    }

    void RenderingManager::setupPlayer(const MWWorld::Ptr& player)
    {
        if (!mPlayerNode)
        {
            mPlayerNode = new SceneUtil::PositionAttitudeTransform;
            mPlayerNode->setNodeMask(Mask_Player);
            mPlayerNode->setName("Player Root");
            mSceneRoot->addChild(mPlayerNode);
        }

        mPlayerNode->setUserDataContainer(new osg::DefaultUserDataContainer);
        mPlayerNode->getUserDataContainer()->addUserObject(new PtrHolder(player));

        player.getRefData().setBaseNode(mPlayerNode);

        mRenderer.removeWaterRippleEmitter(player);
        mRenderer.addWaterRippleEmitter(player);
    }

    void RenderingManager::renderPlayer(const MWWorld::Ptr& player)
    {
        mPlayerAnimation = new NpcAnimation(player, player.getRefData().getBaseNode(), mResourceSystem, 0,
            NpcAnimation::VM_Normal, mFirstPersonFieldOfView);

        mCamera->setAnimation(mPlayerAnimation.get());
        mCamera->attachTo(player);
    }

    void RenderingManager::rebuildPtr(const MWWorld::Ptr& ptr)
    {
        NpcAnimation* anim = nullptr;
        if (ptr == mPlayerAnimation->getPtr())
            anim = mPlayerAnimation.get();
        else
            anim = dynamic_cast<NpcAnimation*>(mObjects->getAnimation(ptr));
        if (anim)
        {
            anim->rebuild();
            if (mCamera->getTrackingPtr() == ptr)
            {
                mCamera->attachTo(ptr);
                mCamera->setAnimation(anim);
            }
        }
    }

    void RenderingManager::addWaterRippleEmitter(const MWWorld::Ptr& ptr)
    {
        mRenderer.addWaterRippleEmitter(ptr);
    }

    void RenderingManager::removeWaterRippleEmitter(const MWWorld::Ptr& ptr)
    {
        mRenderer.removeWaterRippleEmitter(ptr);
    }

    void RenderingManager::emitWaterRipple(const osg::Vec3f& pos)
    {
        mRenderer.emitWaterRipple(pos);
    }

    void RenderingManager::updateProjectionMatrix()
    {
        if (mNearClip < 0.0f)
            throw std::runtime_error("Near clip is less than zero");
        if (mViewDistance < mNearClip)
            throw std::runtime_error("Viewing distance is less than near clip");

        const int width = Settings::video().mResolutionX;
        const int height = Settings::video().mResolutionY;

        const double aspect = (height == 0) ? 1.0 : static_cast<double>(width) / height;
        const float fov = mFieldOfViewOverridden ? mFieldOfViewOverride : mFieldOfView;

        osg::Matrix unreversedProjectionMatrix = osg::Matrix::perspective(fov, aspect, mNearClip, mViewDistance);

        osg::Matrix projectionMatrix = SceneUtil::AutoDepth::isReversed()
            ? SceneUtil::getReversedZProjectionMatrixAsPerspective(fov, aspect, mNearClip, mViewDistance)
            : unreversedProjectionMatrix;

        if (width != 0 && height != 0)
        {
            double offsetX = (mProjectionOffset.x() / width) * 2.0;
            double offsetY = (mProjectionOffset.y() / height) * 2.0;

            const osg::Matrix translation = osg::Matrix::translate(offsetX, offsetY, 0.0);

            projectionMatrix.postMult(translation);
            unreversedProjectionMatrix.postMult(translation);
        }

        // We always set the cameras projection matrix to the un-reversed variant for correct frustum culling.
        mRenderer.getCamera().setProjectionMatrix(unreversedProjectionMatrix);

        mProjectionMatrix = projectionMatrix;

        // Since our fog is not radial yet, we should take FOV in account, otherwise terrain near viewing distance may
        // disappear. Limit FOV here just for sure, otherwise viewing distance can be too high.
        float distanceMult = std::cos(osg::DegreesToRadians(std::min(fov, 140.f)) / 2.f);
        mTerrain->setViewDistance(mViewDistance * (distanceMult ? 1.f / distanceMult : 1.f));
    }

    void RenderingManager::updateTextureFiltering()
    {
        mRenderer.suspendDraw();

        mResourceSystem->getSceneManager()->setFilterSettings(Settings::general().mTextureMagFilter,
            Settings::general().mTextureMinFilter, Settings::general().mTextureMipmap,
            static_cast<float>(Settings::general().mAnisotropy));

        mTerrain->updateTextureFiltering();

        mRenderer.resumeDraw();
    }

    void RenderingManager::updateAmbient()
    {
        osg::Vec4f color = mAmbientColor;

        if (mNightEyeFactor > 0.f)
            color += osg::Vec4f(0.7f, 0.7f, 0.7f, 0.0f) * mNightEyeFactor;

        mSunLight->setAmbient(color);
    }

    RenderingManager::WorldspaceChunkMgr& RenderingManager::getWorldspaceChunkMgr(ESM::RefId worldspace)
    {
        auto existingChunkMgr = mWorldspaceChunks.find(worldspace);
        if (existingChunkMgr != mWorldspaceChunks.end())
            return existingChunkMgr->second;
        RenderingManager::WorldspaceChunkMgr newChunkMgr = mRenderer.createGround(GroundSpec{
            .mSceneRoot = *mSceneRoot,
            .mWorldRoot = *mRootNode,
            .mResources = *mResourceSystem,
            .mStorage = *mTerrainStorage,
            .mGroundcoverStore = mGroundCoverStore,
            .mWorldspace = worldspace,
        });

        float distanceMult = std::cos(osg::DegreesToRadians(std::min(mFieldOfView, 140.f)) / 2.f);
        newChunkMgr.mTerrain->setViewDistance(mViewDistance * (distanceMult ? 1.f / distanceMult : 1.f));

        return mWorldspaceChunks.emplace(worldspace, std::move(newChunkMgr)).first->second;
    }

    void RenderingManager::reportStats() const
    {
        osg::Stats* stats = &mRenderer.getStats();
        unsigned int frameNumber = mRenderer.getFrameStamp().getFrameNumber();
        if (stats->collectStats("resource"))
        {
            mTerrain->reportStats(frameNumber, stats);
        }
    }

    void RenderingManager::processChangedSettings(const Settings::CategorySettingVector& changed)
    {
        // Only perform a projection matrix update once if a relevant setting is changed.
        bool updateProjection = false;

        for (Settings::CategorySettingVector::const_iterator it = changed.begin(); it != changed.end(); ++it)
        {
            if (it->first == "Camera" && it->second == "field of view")
            {
                mFieldOfView = Settings::camera().mFieldOfView;
                updateProjection = true;
            }
            else if (it->first == "Video" && (it->second == "resolution x" || it->second == "resolution y"))
            {
                updateProjection = true;
            }
            else if (it->first == "Camera" && it->second == "viewing distance")
            {
                setViewDistance(Settings::camera().mViewingDistance);
            }
            else if (it->first == "General"
                && (it->second == "texture filter" || it->second == "texture mipmap" || it->second == "anisotropy"))
            {
                updateTextureFiltering();
            }
            else if (it->first == "Shaders" && it->second == "minimum interior brightness")
            {
                if (MWMechanics::getPlayer().isInCell())
                    configureAmbient(*MWMechanics::getPlayer().getCell()->getCell());
            }
            else if (it->first == "Shaders"
                && (it->second == "classic falloff" || it->second == "light radius multiplier"
                    || it->second == "maximum light distance" || it->second == "light fade start"
                    || it->second == "max lights" || it->second == "clustered lighting"
                    || it->second == "particle point lighting"))
            {
                if (MWMechanics::getPlayer().isInCell())
                    configureAmbient(*MWMechanics::getPlayer().getCell()->getCell());
            }
            else if (it->first == "Post Processing" && it->second == "enabled")
            {
                if (!Settings::postProcessing().mEnabled)
                {
                    if (auto* hud = MWBase::Environment::get().getWindowManager()->getPostProcessorHud())
                        hud->setVisible(false);
                }
            }
        }

        mRenderer.processChangedSettings(changed);

        if (updateProjection)
        {
            updateProjectionMatrix();
        }
    }

    void RenderingManager::setViewDistance(float distance, bool delay)
    {
        mViewDistance = distance;

        if (delay)
        {
            mUpdateProjectionMatrix = true;
            return;
        }

        updateProjectionMatrix();
    }

    float RenderingManager::getTerrainHeightAt(const osg::Vec3f& pos, ESM::RefId worldspace)
    {
        return getWorldspaceChunkMgr(worldspace).mTerrain->getHeightAt(pos);
    }

    void RenderingManager::overrideFieldOfView(float val)
    {
        if (mFieldOfViewOverridden != true || mFieldOfViewOverride != val)
        {
            mFieldOfViewOverridden = true;
            mFieldOfViewOverride = val;
            updateProjectionMatrix();
        }
    }

    void RenderingManager::setFieldOfView(float val)
    {
        mFieldOfView = val;
        mUpdateProjectionMatrix = true;
    }

    float RenderingManager::getFieldOfView() const
    {
        return mFieldOfViewOverridden ? mFieldOfViewOverridden : mFieldOfView;
    }

    osg::Vec3f RenderingManager::getHalfExtents(const MWWorld::ConstPtr& object) const
    {
        osg::Vec3f halfExtents(0, 0, 0);
        VFS::Path::Normalized modelName(object.getClass().getCorrectedModel(object));
        if (modelName.empty())
            return halfExtents;

        osg::ref_ptr<const osg::Node> node = mResourceSystem->getSceneManager()->getTemplate(modelName);
        osg::ComputeBoundsVisitor computeBoundsVisitor;
        computeBoundsVisitor.setTraversalMask(~(MWRender::Mask_ParticleSystem | MWRender::Mask_Effect));
        const_cast<osg::Node*>(node.get())->accept(computeBoundsVisitor);
        osg::BoundingBox bounds = computeBoundsVisitor.getBoundingBox();

        if (bounds.valid())
        {
            halfExtents[0] = std::abs(bounds.xMax() - bounds.xMin()) / 2.f;
            halfExtents[1] = std::abs(bounds.yMax() - bounds.yMin()) / 2.f;
            halfExtents[2] = std::abs(bounds.zMax() - bounds.zMin()) / 2.f;
        }

        return halfExtents;
    }

    osg::BoundingBox RenderingManager::getCullSafeBoundingBox(const MWWorld::Ptr& ptr) const
    {
        if (ptr.isEmpty())
            return {};

        osg::ref_ptr<SceneUtil::PositionAttitudeTransform> rootNode = ptr.getRefData().getBaseNode();

        // Recalculate bounds on the ptr's template when the object is not loaded or is loaded but paged
        MWWorld::Scene* worldScene = MWBase::Environment::get().getWorldScene();
        if (!rootNode || worldScene->isPagedRef(ptr))
        {
            const VFS::Path::Normalized model(ptr.getClass().getCorrectedModel(ptr));

            if (model.empty())
                return {};

            rootNode = new SceneUtil::PositionAttitudeTransform;
            // Hack even used by osg internally, osg's NodeVisitor won't accept const qualified nodes
            rootNode->addChild(const_cast<osg::Node*>(mResourceSystem->getSceneManager()->getTemplate(model).get()));

            const float refScale = ptr.getCellRef().getScale();
            rootNode->setScale({ refScale, refScale, refScale });
            const auto& rotation = ptr.getCellRef().getPosition().rot;
            if (!ptr.getClass().isActor())
                rootNode->setAttitude(osg::Quat(rotation[0], osg::Vec3(-1, 0, 0))
                    * osg::Quat(rotation[1], osg::Vec3(0, -1, 0)) * osg::Quat(rotation[2], osg::Vec3(0, 0, -1)));
            rootNode->setPosition(ptr.getCellRef().getPosition().asVec3());

            osg::ref_ptr<Animation> animation = nullptr;

            if (ptr.getClass().isNpc())
            {
                rootNode->setNodeMask(Mask_Actor);
                animation = new NpcAnimation(ptr, osg::ref_ptr<osg::Group>(rootNode), mResourceSystem);
            }
        }

        SceneUtil::CullSafeBoundsVisitor computeBounds;
        computeBounds.setTraversalMask(~(MWRender::Mask_ParticleSystem | MWRender::Mask_Effect));
        rootNode->accept(computeBounds);

        return computeBounds.mBoundingBox;
    }

    void RenderingManager::resetFieldOfView()
    {
        if (mFieldOfViewOverridden == true)
        {
            mFieldOfViewOverridden = false;

            updateProjectionMatrix();
        }
    }
    void RenderingManager::exportSceneGraph(
        const MWWorld::Ptr& ptr, const std::filesystem::path& filename, const std::string& format)
    {
        osg::Node* node = &mRenderer.getTraversalRoot();
        if (!ptr.isEmpty())
            node = ptr.getRefData().getBaseNode();

        SceneUtil::writeScene(node, filename, format);
    }

    LandManager* RenderingManager::getLandManager() const
    {
        return mTerrainStorage->getLandManager();
    }

    void RenderingManager::updateActorPath(const MWWorld::ConstPtr& actor, const std::deque<osg::Vec3f>& path,
        const DetourNavigator::AgentBounds& agentBounds, const osg::Vec3f& start, const osg::Vec3f& end) const
    {
        mActorsPaths->update(actor, path, agentBounds, start, end, mNavigator.getSettings());
    }

    void RenderingManager::removeActorPath(const MWWorld::ConstPtr& actor) const
    {
        mActorsPaths->remove(actor);
    }

    void RenderingManager::setNavMeshNumber(const std::size_t value)
    {
        mNavMeshNumber = value;
    }

    void RenderingManager::updateNavMesh()
    {
        if (!mNavMesh->isEnabled())
            return;

        const auto navMeshes = mNavigator.getNavMeshes();

        auto it = navMeshes.begin();
        for (std::size_t i = 0; it != navMeshes.end() && i < mNavMeshNumber; ++i)
            ++it;
        if (it == navMeshes.end())
        {
            mNavMesh->reset();
        }
        else
        {
            try
            {
                mNavMesh->update(it->second, mNavMeshNumber, mNavigator.getSettings());
            }
            catch (const std::exception& e)
            {
                Log(Debug::Error) << "NavMesh render update exception: " << e.what();
            }
        }
    }

    void RenderingManager::updateRecastMesh()
    {
        if (!mRecastMesh->isEnabled())
            return;

        mRecastMesh->update(mNavigator.getRecastMeshTiles(), mNavigator.getSettings());
    }

    void RenderingManager::setActiveGrid(const osg::Vec4i& grid)
    {
        mTerrain->setActiveGrid(grid);
    }
    bool RenderingManager::pagingEnableObject(int type, const MWWorld::ConstPtr& ptr, bool enabled)
    {
        if (!ptr.isInCell() || !ptr.getCell()->isExterior())
            return false;
        mRenderer.enableReference(ptr.getCellRef().getRefNum(), enabled);
        if (!mObjectPaging)
            return false;
        if (mObjectPaging->enableObject(type, ptr.getCellRef().getRefNum(), ptr.getCellRef().getPosition().asVec3(),
                osg::Vec2i(ptr.getCell()->getCell()->getGridX(), ptr.getCell()->getCell()->getGridY()), enabled))
        {
            mTerrain->rebuildViews();
            return true;
        }
        return false;
    }
    void RenderingManager::pagingBlacklistObject(int type, const MWWorld::ConstPtr& ptr)
    {
        if (!ptr.isInCell() || !ptr.getCell()->isExterior() || !mObjectPaging)
            return;
        ESM::RefNum refnum = ptr.getCellRef().getRefNum();
        if (!refnum.hasContentFile())
            return;
        if (mObjectPaging->blacklistObject(type, refnum, ptr.getCellRef().getPosition().asVec3(),
                osg::Vec2i(ptr.getCell()->getCell()->getGridX(), ptr.getCell()->getCell()->getGridY())))
            mTerrain->rebuildViews();
    }
    bool RenderingManager::pagingUnlockCache()
    {
        if (mObjectPaging && mObjectPaging->unlockCache())
        {
            mTerrain->rebuildViews();
            return true;
        }
        return false;
    }
    void RenderingManager::getPagedRefnums(const osg::Vec4i& activeGrid, std::vector<ESM::RefNum>& out)
    {
        if (mObjectPaging)
            mObjectPaging->getPagedRefnums(activeGrid, out);
    }

    void RenderingManager::setNavMeshMode(Settings::NavMeshRenderMode value)
    {
        mNavMesh->setMode(value);
    }
}
