#include "glworld.hpp"

#include <string>

#include <osg/Camera>
#include <osg/ClipControl>
#include <osg/Group>
#include <osg/NodeVisitor>
#include <osg/Uniform>

#include <osgViewer/Viewer>

#include <components/debug/debugdraw.hpp>
#include <components/fx/stateupdater.hpp>
#include <components/misc/constants.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/rtt.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/sceneutil/stateupdater.hpp>
#include <components/sceneutil/texmat.hpp>
#include <components/settings/values.hpp>
#include <components/shader/removedalphafunc.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/stereo/stereomanager.hpp>
#include <components/terrain/world.hpp>

#include "postprocessor.hpp"
#include "precipitationocclusion.hpp"
#include "renderingmanager.hpp"
#include "sky.hpp"
#include "vismask.hpp"
#include "water.hpp"

// Upstream's, from renderingmanager.cpp.
namespace
{
    class LightManagerUpdateVisitor : public osg::NodeVisitor
    {
    public:
        LightManagerUpdateVisitor()
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
            setNodeMaskOverride(~0u);
        }

        void apply(osg::Node& node) override
        {
            if (auto* rtt = dynamic_cast<SceneUtil::RTTNode*>(&node))
            {
                for (const auto& [_, vdd] : rtt->getViewDependentDataMap())
                {
                    traverse(*vdd->mCamera.get());
                }
            }

            traverse(node);
        }

        void apply(osg::Group& node) override
        {
            if (auto* lm = dynamic_cast<SceneUtil::LightManager*>(&node))
            {
                if (mDoThreadUnsafeOps)
                {
                    lm->updateMaxLights(Settings::shaders().mMaxLights);
                    lm->enableClustered(Settings::shaders().mClusteredLighting);
                }

                lm->processChangedSettings(Settings::shaders().mLightRadiusMultiplier,
                    Settings::shaders().mMaximumLightDistance, Settings::shaders().mLightFadeStart);

                return;
            }
            traverse(node);
        }

        void setDoThreadUnsafeOps(bool doThreadUnsafeOps) { mDoThreadUnsafeOps = doThreadUnsafeOps; }

    private:
        bool mDoThreadUnsafeOps = false;
    };

    unsigned int getIndoorShadowCastingMask()
    {
        unsigned int mask = MWRender::Mask_Scene;
        if (Settings::shadows().mActorShadows)
            mask |= MWRender::Mask_Actor;
        if (Settings::shadows().mPlayerShadows)
            mask |= MWRender::Mask_Player;
        return mask;
    }

    unsigned int getOutdoorShadowCastingMask()
    {
        unsigned int mask = getIndoorShadowCastingMask();
        if (Settings::shadows().mObjectShadows)
            mask |= (MWRender::Mask_Object | MWRender::Mask_Static);
        if (Settings::shadows().mTerrainShadows)
            mask |= MWRender::Mask_Terrain;
        return mask;
    }
}

namespace MWRender
{
    GlWorld::GlWorld(osgViewer::Viewer& viewer, RenderingManager& world, osg::Group& worldRoot,
        SceneUtil::LightManager& sceneRoot, Resource::ResourceSystem& resources)
        : mViewer(viewer)
        , mResources(resources)
        , mSceneRoot(&sceneRoot)
    {
        Resource::SceneManager& scene = *resources.getSceneManager();
        const bool skyBlending = Settings::fog().mSkyBlending;
        bool reverseZ = SceneUtil::AutoDepth::isReversed();

        mShadowManager = std::make_unique<SceneUtil::ShadowManager>(&sceneRoot, &worldRoot,
            getOutdoorShadowCastingMask(), getIndoorShadowCastingMask(), Mask_Terrain | Mask_Object | Mask_Static,
            Settings::shadows(), scene.getShaderManager());

        Shader::ShaderManager::DefineMap globalDefines = Shader::getDefaultDefines();
        mAppliedShadowDefines = mShadowManager->getShadowDefines(Settings::shadows());
        Shader::ShaderManager::DefineMap lightDefines = sceneRoot.getLightDefines();

        for (const auto& [key, value] : mAppliedShadowDefines)
            globalDefines[key] = value;

        globalDefines["forcePPL"] = Settings::shaders().mForcePerPixelLighting ? "1" : "0";
        globalDefines["clamp"] = Settings::shaders().mClampLighting ? "1" : "0";
        globalDefines["preLightEnv"] = Settings::shaders().mApplyLightingToEnvironmentMaps ? "1" : "0";
        globalDefines["classicFalloff"] = Settings::shaders().mClassicFalloff ? "1" : "0";
        const bool exponentialFog = Settings::fog().mExponentialFog;
        globalDefines["radialFog"] = (exponentialFog || Settings::fog().mRadialFog) ? "1" : "0";
        globalDefines["exponentialFog"] = exponentialFog ? "1" : "0";
        globalDefines["skyBlending"] = skyBlending ? "1" : "0";
        globalDefines["particlePointLighting"] = Settings::shaders().mParticlePointLighting ? "1" : "0";

        for (auto itr = lightDefines.begin(); itr != lightDefines.end(); itr++)
            globalDefines[itr->first] = itr->second;

        // Refactor this at some point - most shaders don't care about these defines
        const float groundcoverDistance = Settings::groundcover().mRenderingDistance;
        globalDefines["groundcoverFadeStart"] = std::to_string(groundcoverDistance * 0.9f);
        globalDefines["groundcoverFadeEnd"] = std::to_string(groundcoverDistance);
        globalDefines["groundcoverStompMode"] = std::to_string(Settings::groundcover().mStompMode);
        globalDefines["groundcoverStompIntensity"] = std::to_string(Settings::groundcover().mStompIntensity);

        globalDefines["reverseZ"] = reverseZ ? "1" : "0";

        // It is unnecessary to stop/start the viewer as no frames are being rendered yet.
        scene.getShaderManager().setGlobalDefines(globalDefines);

        // Only set up the shadow casting shaders once our global defines have been set
        mShadowManager->setupShaders(scene.getShaderManager());

        mDebugDraw = new Debug::DebugDrawer(scene.getShaderManager());
        mDebugDraw->setNodeMask(Mask_Debug);
        sceneRoot.addChild(mDebugDraw);

        mStateUpdater = new SceneUtil::StateUpdater();
        sceneRoot.addUpdateCallback(mStateUpdater);

        mSharedUniformStateUpdater = new SceneUtil::SharedUniformStateUpdater(Settings::fog().mSkyBlendingStart);
        worldRoot.addUpdateCallback(mSharedUniformStateUpdater);

        mPerViewUniformStateUpdater = new SceneUtil::PerViewUniformStateUpdater(&scene,
            scene.getShaderManager().reserveGlobalTextureUnits(Shader::ShaderManager::Slot::OpaqueDepthTexture),
            scene.getShaderManager().reserveGlobalTextureUnits(Shader::ShaderManager::Slot::OpaqueColorTexture));
        worldRoot.addCullCallback(mPerViewUniformStateUpdater);

        // The dome, and the occluder that keeps the rain out from under roofs: upstream's sky manager
        // less the precipitation, which the game keeps because both renderers draw it. Before the
        // chain, which reads the sky for its sun glare.
        mSky = std::make_unique<SkyManager>(&sceneRoot, &scene, skyBlending);

        osg::Group& precipitationRoot = world.getPrecipitationRoot();
        scene.setUpNormalsRTForStateSet(precipitationRoot.getOrCreateStateSet(), false);
        SceneUtil::ShadowManager::instance().disableShadowsForStateSet(*precipitationRoot.getOrCreateStateSet());

        mPrecipitationOcclusion = Settings::shaders().mWeatherParticleOcclusion;
        mPrecipitationOccluder
            = std::make_unique<PrecipitationOccluder>(&precipitationRoot, &sceneRoot, &worldRoot, viewer.getCamera());

        // **The chain goes above the world and becomes what is traversed.** Its constructor reads
        // `GLExtensions` off the camera's graphics context, which is why no renderer without one can
        // have it and why nothing above this line decides whether to build it.
        mPostProcessor = new PostProcessor(world, &viewer, &worldRoot, resources.getVFS(), sceneRoot, *mSky);

        scene.setOpaqueDepthTex(mPostProcessor->getTexture(PostProcessor::Tex_OpaqueDepth, 0),
            mPostProcessor->getTexture(PostProcessor::Tex_OpaqueDepth, 1));
        scene.setOpaqueColorTex(mPostProcessor->getTexture(PostProcessor::Tex_OpaqueColor, 0),
            mPostProcessor->getTexture(PostProcessor::Tex_OpaqueColor, 1));
        scene.setSupportsNormalsRT(mPostProcessor->getSupportsNormalsRT());

        // water goes after terrain for correct waterculling order
        mWater = std::make_unique<Water>(
            sceneRoot.getParent(0), &sceneRoot, &resources, viewer.getIncrementalCompileOperation());
        mPostProcessor->setupTransparentBin(mWater.get());

        sceneRoot.setSunlight(&world.getSunLight());

        scene.setUpNormalsRTForStateSet(sceneRoot.getOrCreateStateSet(), true);

        if (skyBlending)
        {
            int skyTextureUnit
                = scene.getShaderManager().reserveGlobalTextureUnits(Shader::ShaderManager::Slot::SkyTexture);
            mPerViewUniformStateUpdater->enableSkyRTT(skyTextureUnit, mSky->getSkyRTT());
        }

        applyRootState(worldRoot, sceneRoot, *viewer.getCamera());
    }

    GlWorld::~GlWorld() = default;

    void GlWorld::addCell(const MWWorld::CellStore* cell)
    {
        mWater->changeCell(cell);
    }

    void GlWorld::removeCell(const MWWorld::CellStore* cell)
    {
        mWater->removeCell(cell);
    }

    void GlWorld::addWaterRippleEmitter(const MWWorld::Ptr& ptr)
    {
        mWater->addEmitter(ptr);
    }

    void GlWorld::removeWaterRippleEmitter(const MWWorld::Ptr& ptr)
    {
        mWater->removeEmitter(ptr);
    }

    void GlWorld::emitWaterRipple(const osg::Vec3f& position)
    {
        mWater->emitRipple(position);
    }

    void GlWorld::clearRipples()
    {
        mWater->clearRipples();
    }

    void GlWorld::listAssetsToPreload(
        std::vector<VFS::Path::Normalized>& models, std::vector<VFS::Path::Normalized>& textures)
    {
        mSky->listAssetsToPreload(models, textures);
        mWater->listAssetsToPreload(textures);
    }

    void GlWorld::setWorldShown(const bool shown)
    {
        mWater->showWorld(shown);
    }

    void GlWorld::applyRootState(osg::Group& worldRoot, SceneUtil::LightManager& sceneRoot, osg::Camera& camera)
    {
        bool reverseZ = SceneUtil::AutoDepth::isReversed();

        sceneRoot.getOrCreateStateSet()->setMode(GL_CULL_FACE, osg::StateAttribute::ON);
        sceneRoot.getOrCreateStateSet()->setMode(GL_NORMALIZE, osg::StateAttribute::ON);
        sceneRoot.getOrCreateStateSet()->addUniform(new osg::Uniform("distortionStrength", 0.f));
        sceneRoot.getOrCreateStateSet()->addUniform(new osg::Uniform("alpha", 1.f));
        sceneRoot.getOrCreateStateSet()->addUniform(new osg::Uniform("actorFade", 1.f));

        osg::Camera::CullingMode cullingMode = osg::Camera::DEFAULT_CULLING | osg::Camera::FAR_PLANE_CULLING;

        if (!Settings::camera().mSmallFeatureCulling)
            cullingMode &= ~(osg::CullStack::SMALL_FEATURE_CULLING);
        else
        {
            camera.setSmallFeatureCullingPixelSize(Settings::camera().mSmallFeatureCullingPixelSize);
            cullingMode |= osg::CullStack::SMALL_FEATURE_CULLING;
        }

        camera.setComputeNearFarMode(osg::Camera::DO_NOT_COMPUTE_NEAR_FAR);
        camera.setCullingMode(cullingMode);
        camera.setName(Constants::SceneCamera);

        // Hopefully, anything genuinely requiring the default alpha func of GL_ALWAYS explicitly sets it
        mPostProcessor->getOrCreateStateSet()->setAttribute(Shader::RemovedAlphaFunc::getInstance(GL_ALWAYS));
        // The transparent renderbin sets alpha testing on because that was faster on old GPUs. It's now slower and
        // breaks things.
        worldRoot.getOrCreateStateSet()->setMode(GL_ALPHA_TEST, osg::StateAttribute::OFF);

        if (reverseZ)
        {
            osg::ref_ptr<osg::ClipControl> clipcontrol
                = new osg::ClipControl(osg::ClipControl::LOWER_LEFT, osg::ClipControl::ZERO_TO_ONE);
            worldRoot.getOrCreateStateSet()->setAttributeAndModes(new SceneUtil::AutoDepth, osg::StateAttribute::ON);
            worldRoot.getOrCreateStateSet()->setAttributeAndModes(clipcontrol, osg::StateAttribute::ON);
        }

        SceneUtil::initTexMatForStateSet(*mPostProcessor->getOrCreateStateSet());

        worldRoot.getOrCreateStateSet()->setMode(
            GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::PROTECTED | osg::StateAttribute::OVERRIDE);

        SceneUtil::setCameraClearDepth(&camera);

        camera.setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }

    void GlWorld::describe(const SceneFrame& frame)
    {
        const WorldState& world = frame.mWorld;
        const EyeState& eye = frame.mEye;

        Fx::StateUpdater& state = *mPostProcessor->getStateUpdater();
        state.setSunPos(world.mSunPosition, world.mSunAtNight);
        state.setSunVec(world.mSunVector);
        state.setSunColor(world.mSunColour);
        state.setSunVis(world.mSunVisibility);
        state.setAmbientColor(world.mAmbientColour);
        state.setSkyColor(world.mSkyColour);
        state.setIsInterior(world.isInteriorCell());
        state.setIsWaterEnabled(world.mWaterEnabled);
        state.setWaterHeight(world.mWaterHeight);
        state.setIsUnderwater(world.mUnderwater);
        const FogBand& fog = world.mUnderwater ? world.mWaterFog : world.mAir;
        state.setFogColor(fog.mColour);
        state.setFogRange(fog.mStart, fog.mEnd);
        state.setNearFar(eye.mNearClip, eye.mViewDistance);
        state.setProjectionMatrix(eye.mProjectionMatrix);
        state.setFov(eye.mFieldOfView);
        state.setGameHour(world.mGameHour);
        state.setWeatherId(world.mWeatherId);
        // -1 for no transition, which is what the world hands over and what a technique reads.
        state.setNextWeatherId(world.mNextWeatherId.value_or(-1));
        state.setWeatherTransition(world.mWeatherTransition);
        state.setWindSpeed(world.mWindSpeed);
        // Which techniques run at all. A quasi-exterior is outside here and inside for the
        // `isInterior` uniform above, which is why `WorldState` answers the two apart.
        mPostProcessor->setUnderwaterFlag(world.mUnderwater);
        mPostProcessor->setExteriorFlag(world.isOutdoors());

        mStateUpdater->setFogColor(world.mAir.mColour);
        mStateUpdater->setFogStart(world.mAir.mStart);
        mStateUpdater->setFogEnd(world.mAir.mEnd);
        mStateUpdater->setUnderwaterFogStart(world.mWaterFog.mStart);
        mStateUpdater->setUnderwaterFogEnd(world.mWaterFog.mEnd);
        mStateUpdater->setUnderwaterFogColor(world.mWaterFog.mColour);
        mStateUpdater->setWaterEnabled(world.mWaterEnabled);
        mStateUpdater->setWaterHeight(world.mWaterHeight);
        mStateUpdater->setAmbientColor(world.mAmbientColour);

        if (!frame.mPaused)
        {
            mSharedUniformStateUpdater->setWindSpeed(world.mBaseWindSpeed);
            mSharedUniformStateUpdater->setPlayerPos(world.mPlayerPosition);
        }

        mSharedUniformStateUpdater->setNear(eye.mNearClip);
        mSharedUniformStateUpdater->setFar(eye.mViewDistance);
        mPerViewUniformStateUpdater->setProjectionMatrix(eye.mProjectionMatrix);

        // Upstream's, from updateProjectionMatrix: an eye's picture is the eye's size
        if (Stereo::getStereo())
        {
            auto res = Stereo::Manager::instance().eyeResolution();
            mSharedUniformStateUpdater->setScreenRes(static_cast<float>(res.x()), static_cast<float>(res.y()));
            Stereo::Manager::instance().setMasterProjectionMatrix(eye.mProjectionMatrix);
        }
        else
            mSharedUniformStateUpdater->setScreenRes(
                static_cast<float>(eye.mScreenResolution.x()), static_cast<float>(eye.mScreenResolution.y()));

        mViewer.getCamera()->setClearColor(fog.mColour);

        // **The dome, fed as the weather manager fed it**: every setter per frame, in the weather
        // manager's order — a moon's state sets its transparency and the weather then scales it —
        // and only while there is a sky, because the setters want a built dome and `setEnabled(true)`
        // is what builds it. The weather is null until the weather has run, which it has whenever
        // the sky is on.
        if (!mApplied.mAny || mApplied.mSkyEnabled != world.mSkyEnabled)
        {
            mSky->setEnabled(world.mSkyEnabled);
            mApplied.mSkyEnabled = world.mSkyEnabled;
        }
        if (world.mSkyEnabled && world.mWeather != nullptr)
        {
            if (world.mSunEnabled)
                mSky->sunEnable();
            else
                mSky->sunDisable();
            mSky->setSunDirection(osg::Vec3f(world.mSunPosition.x(), world.mSunPosition.y(), world.mSunPosition.z()));
            mSky->setGlareTimeOfDayFade(world.mGlareFade);
            mSky->setMasserState(world.mMoons[0]);
            mSky->setSecundaState(world.mMoons[1]);
            mSky->setWeather(*world.mWeather);
            mSky->setMoonColour(world.mMoonRed);
        }

        // The occluder as the sky manager drove it: enabled where the precipitation says it was,
        // its range whenever it is on, and stepped after the dome each unpaused frame. A fresh one
        // is disabled, which is what `Applied` starts at.
        if (mPrecipitationOcclusion && mApplied.mPrecipitating != world.mPrecipitating)
        {
            if (world.mPrecipitating)
                mPrecipitationOccluder->enable();
            else
                mPrecipitationOccluder->disable();
            mApplied.mPrecipitating = world.mPrecipitating;
        }
        if (mPrecipitationOcclusion && world.mPrecipitating)
            mPrecipitationOccluder->updateRange(world.mPrecipitationRange);

        if (!frame.mPaused && world.mSkyEnabled)
        {
            mSky->update(world.mCloudScroll, world.mStarRoll);
            mPrecipitationOccluder->update();
        }

        // The shadow technique's mode is a rebuild, so it follows the door and not the frame
        if (!mApplied.mAny || mApplied.mOutdoors != world.isOutdoors())
        {
            if (world.isOutdoors())
                mShadowManager->enableOutdoorMode();
            else
                mShadowManager->enableIndoorMode(Settings::shadows());
            mApplied.mOutdoors = world.isOutdoors();
        }

        // Upstream's setWaterHeight and enableTerrain: the water is culled by the ground's height
        // where there is ground, and the terrain hands the cull out only once it has chunks
        const bool exterior = world.mLocation == Location::Exterior;
        if (!mApplied.mAny || mApplied.mWaterHeight != world.mWaterHeight || mApplied.mExterior != exterior
            || (exterior && !mApplied.mWaterCulled))
        {
            osg::Callback* cull
                = exterior ? frame.mTerrain.getHeightCullCallback(world.mWaterHeight, Mask_Water) : nullptr;
            mWater->setCullCallback(cull);
            mApplied.mWaterCulled = !exterior || cull != nullptr;
            mApplied.mExterior = exterior;
        }
        if (!mApplied.mAny || mApplied.mWaterHeight != world.mWaterHeight)
        {
            mWater->setHeight(world.mWaterHeight);
            mApplied.mWaterHeight = world.mWaterHeight;
        }
        if (!mApplied.mAny || mApplied.mWaterEnabled != world.mWaterEnabled)
        {
            mWater->setEnabled(world.mWaterEnabled);
            mApplied.mWaterEnabled = world.mWaterEnabled;
        }

        mWater->setRainIntensity(world.mRainOnWater);
        mWater->update(frame.mDeltaTime, frame.mPaused);

        mApplied.mAny = true;
    }

    bool GlWorld::toggleWireframe()
    {
        bool wireframe = !mStateUpdater->getWireframe();
        mStateUpdater->setWireframe(wireframe);
        return wireframe;
    }

    // Upstream's, from RenderingManager::processChangedSettings: the branches over what is made here.
    void GlWorld::processChangedSettings(const Settings::CategorySettingVector& changed)
    {
        Resource::SceneManager& scene = *mResources.getSceneManager();

        for (Settings::CategorySettingVector::const_iterator it = changed.begin(); it != changed.end(); ++it)
        {
            if (it->first == "Shaders"
                && (it->second == "force per pixel lighting" || it->second == "classic falloff"
                    || it->second == "clamp lighting"))
            {
                mViewer.stopThreading();

                auto defines = scene.getShaderManager().getGlobalDefines();
                defines["forcePPL"] = Settings::shaders().mForcePerPixelLighting ? "1" : "0";
                defines["classicFalloff"] = Settings::shaders().mClassicFalloff ? "1" : "0";
                defines["clamp"] = Settings::shaders().mClampLighting ? "1" : "0";
                scene.getShaderManager().setGlobalDefines(defines);

                mViewer.startThreading();
            }
            else if (it->first == "Shaders"
                && (it->second == "light radius multiplier" || it->second == "maximum light distance"
                    || it->second == "light fade start" || it->second == "max lights"
                    || it->second == "clustered lighting" || it->second == "particle point lighting"))
            {
                LightManagerUpdateVisitor visitor;
                bool lightManagersUpdated = false;

                if (it->second == "max lights" || it->second == "clustered lighting"
                    || it->second == "particle point lighting")
                {
                    mViewer.stopThreading();

                    visitor.setDoThreadUnsafeOps(true);
                    mViewer.getSceneData()->accept(visitor);
                    lightManagersUpdated = true;

                    auto defines = scene.getShaderManager().getGlobalDefines();
                    for (const auto& [name, key] : mSceneRoot->getLightDefines())
                        defines[name] = key;
                    defines["particlePointLighting"] = Settings::shaders().mParticlePointLighting ? "1" : "0";
                    scene.getShaderManager().setGlobalDefines(defines);

                    mStateUpdater->reset();

                    mViewer.startThreading();
                }

                if (!lightManagersUpdated)
                    mViewer.getSceneData()->accept(visitor);
            }
            else if (it->first == "Shadows")
            {
                mShadowManager->setupShadowSettings(Settings::shadows(), scene.getShaderManager());
                mShadowManager->setIndoorShadowCastingMask(getIndoorShadowCastingMask());
                mShadowManager->setOutdoorShadowCastingMask(getOutdoorShadowCastingMask());
                if (mApplied.mOutdoors)
                    mShadowManager->enableOutdoorMode();
                else
                    mShadowManager->enableIndoorMode(Settings::shadows());

                Shader::ShaderManager::DefineMap shadowDefines = mShadowManager->getShadowDefines(Settings::shadows());
                if (mAppliedShadowDefines != shadowDefines)
                {
                    auto defines = scene.getShaderManager().getGlobalDefines();
                    for (const auto& [key, value] : mAppliedShadowDefines)
                        defines.erase(key);
                    for (const auto& [key, value] : shadowDefines)
                        defines[key] = value;
                    mViewer.stopThreading();
                    scene.getShaderManager().setGlobalDefines(defines);
                    mViewer.startThreading();
                    mAppliedShadowDefines = std::move(shadowDefines);
                }
            }
            else if (it->first == "Post Processing" && it->second == "enabled")
            {
                if (Settings::postProcessing().mEnabled)
                    mPostProcessor->enable();
                else
                    mPostProcessor->disable();
            }
            else if (it->first == "Water")
            {
                mWater->processChangedSettings(changed);
            }
            else if (it->first == "General"
                && (it->second == "texture filter" || it->second == "texture mipmap" || it->second == "anisotropy"))
            {
                // The game refilters its textures under a stopped viewer; the water rebuilds its
                // material the same way
                mViewer.stopThreading();
                mWater->processChangedSettings({});
                mViewer.startThreading();
            }
        }
    }
}
