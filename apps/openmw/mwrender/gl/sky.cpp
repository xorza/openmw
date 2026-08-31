#include "sky.hpp"

#include <osg/Depth>
#include <osg/PositionAttitudeTransform>

#include <osgParticle/BoxPlacer>
#include <osgParticle/ModularEmitter>
#include <osgParticle/ModularProgram>
#include <osgParticle/Operator>
#include <osgParticle/ParticleSystemUpdater>

#include <components/sceneutil/vismask.hpp>
#include <components/settings/values.hpp>

#include <components/sceneutil/controller.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/rtt.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/sceneutil/visitor.hpp>

#include <components/resource/imagemanager.hpp>
#include <components/resource/scenemanager.hpp>

#include <components/vfs/manager.hpp>

#include <components/misc/resourcehelpers.hpp>
#include <components/stereo/stereomanager.hpp>

#include <components/nifosg/particle.hpp>

#include "../../mwworld/datetimemanager.hpp"
#include "../../mwworld/weather.hpp"

#include "../../mwbase/environment.hpp"
#include "../../mwbase/world.hpp"

#include "../renderbin.hpp"
#include "../util.hpp"
#include "skyutil.hpp"
#include <components/weather/precipitation.hpp>

namespace
{

    class SkyRTT : public SceneUtil::RTTNode
    {
    public:
        SkyRTT(osg::Vec2f size, osg::Group* earlyRenderBinRoot)
            : RTTNode(static_cast<int>(size.x()), static_cast<int>(size.y()), 0, false, 1, StereoAwareness::Aware,
                MWRender::shouldAddMSAAIntermediateTarget())
            , mEarlyRenderBinRoot(earlyRenderBinRoot)
        {
            setDepthBufferInternalFormat(GL_DEPTH24_STENCIL8);
        }

        void setDefaults(osg::Camera* camera) override
        {
            camera->setReferenceFrame(osg::Camera::RELATIVE_RF);
            camera->setName("SkyCamera");
            camera->setNodeMask(SceneUtil::Mask_RenderToTexture);
            camera->setCullMask(SceneUtil::Mask_Sky);
            camera->addChild(mEarlyRenderBinRoot);
            SceneUtil::ShadowManager::instance().disableShadowsForStateSet(*camera->getOrCreateStateSet());
        }

    private:
        osg::ref_ptr<osg::Group> mEarlyRenderBinRoot;
    };

}

namespace MWRender
{
    SkyManager::SkyManager(osg::Group* parentNode, osg::Group* rootNode, osg::Camera* camera,
        Resource::SceneManager* sceneManager, bool enableSkyRTT)
        : mSceneManager(sceneManager)
        , mCamera(camera)
        , mCreated(false)
        , mIsStorm(false)
        , mStormDirection(Weather::defaultStormDirection())
        , mClouds()
        , mNextClouds()
        , mCloudBlendFactor(0.f)
        , mStarsOpacity(0.f)
        , mWindSpeed(0.f)
        , mBaseWindSpeed(0.f)
        , mEnabled(true)
        , mSunglareEnabled(true)
        , mDirtyParticlesEffect(false)
    {
        mSkyRootNode = new CameraRelativeTransform;
        mSkyRootNode->setName("Sky Root");
        mSceneManager->setUpNormalsRTForStateSet(mSkyRootNode->getOrCreateStateSet(), false);
        SceneUtil::ShadowManager::instance().disableShadowsForStateSet(*mSkyRootNode->getOrCreateStateSet());
        parentNode->addChild(mSkyRootNode);

        mEarlyRenderBinRoot = new osg::Group;
        // render before the world is rendered
        mEarlyRenderBinRoot->getOrCreateStateSet()->setRenderBinDetails(RenderBin_Sky, "RenderBin");
        // Prevent unwanted clipping by water reflection camera's clipping plane
        mEarlyRenderBinRoot->getOrCreateStateSet()->setMode(GL_CLIP_PLANE0, osg::StateAttribute::OFF);

        if (enableSkyRTT)
        {
            mSkyRTT = new SkyRTT(Settings::fog().mSkyRttResolution, mEarlyRenderBinRoot);
            mSkyRootNode->addChild(mSkyRTT);
        }

        mSkyNode = new osg::Group;
        mSkyNode->setNodeMask(SceneUtil::Mask_Sky);
        mSkyNode->addChild(mEarlyRenderBinRoot);
        mSkyRootNode->addChild(mSkyNode);

        mUnderwaterSwitch = new UnderwaterSwitchCallback(mSkyRootNode);

        // **Under the sky's own node, and so behind its mask.** The rain is a box that follows the
        // eye, which is what the camera-relative root above gives it — and a walk that took this
        // subtree along with the world would place every drop where the world's origin is. Whoever
        // wants it asks for it and walks it on its own terms; `SceneUtil::Mask_WeatherParticles` is what says
        // which part of it they meant.
        mPrecipitation
            = std::make_unique<Weather::Precipitation>(mSkyNode, *mSceneManager, SceneUtil::Mask_WeatherParticles);

        mPrecipitationOcclusion = Settings::shaders().mWeatherParticleOcclusion;
        mPrecipitationOccluder = std::make_unique<PrecipitationOccluder>(mSkyRootNode, parentNode, rootNode, camera);
    }

    void SkyManager::create()
    {
        assert(!mCreated);

        mAtmosphereDay = mSceneManager->getInstance(Settings::models().mSkyatmosphere.get(), mEarlyRenderBinRoot);
        ModVertexAlphaVisitor modAtmosphere(ModVertexAlphaVisitor::Atmosphere);
        mAtmosphereDay->accept(modAtmosphere);

        mAtmosphereUpdater = new AtmosphereUpdater;
        mAtmosphereDay->addUpdateCallback(mAtmosphereUpdater);

        mAtmosphereNightNode = new osg::PositionAttitudeTransform;
        mAtmosphereNightNode->setNodeMask(0);
        mEarlyRenderBinRoot->addChild(mAtmosphereNightNode);

        osg::ref_ptr<osg::Node> atmosphereNight;
        if (mSceneManager->getVFS()->exists(Settings::models().mSkynight02.get()))
            atmosphereNight = mSceneManager->getInstance(Settings::models().mSkynight02.get(), mAtmosphereNightNode);
        else
            atmosphereNight = mSceneManager->getInstance(Settings::models().mSkynight01.get(), mAtmosphereNightNode);

        ModVertexAlphaVisitor modStars(ModVertexAlphaVisitor::Stars);
        atmosphereNight->accept(modStars);
        mAtmosphereNightUpdater = new AtmosphereNightUpdater(mSceneManager->getImageManager());
        atmosphereNight->addUpdateCallback(mAtmosphereNightUpdater);

        mSun = std::make_unique<Sun>(mEarlyRenderBinRoot, *mSceneManager);
        mSun->setSunglare(mSunglareEnabled);
        mMasser = std::make_unique<Moon>(
            mEarlyRenderBinRoot, *mSceneManager, Fallback::Map::getFloat("Moons_Masser_Size") / 125, Moon::Type_Masser);
        mSecunda = std::make_unique<Moon>(mEarlyRenderBinRoot, *mSceneManager,
            Fallback::Map::getFloat("Moons_Secunda_Size") / 125, Moon::Type_Secunda);

        mCloudNode = new osg::Group;
        mEarlyRenderBinRoot->addChild(mCloudNode);

        mCloudMesh = new osg::PositionAttitudeTransform;
        osg::ref_ptr<osg::Node> cloudMeshChild
            = mSceneManager->getInstance(Settings::models().mSkyclouds.get(), mCloudMesh);
        mCloudUpdater = new CloudUpdater();
        mCloudUpdater->setOpacity(1.f);
        cloudMeshChild->addUpdateCallback(mCloudUpdater);
        mCloudMesh->addChild(cloudMeshChild);

        mNextCloudMesh = new osg::PositionAttitudeTransform;
        osg::ref_ptr<osg::Node> nextCloudMeshChild
            = mSceneManager->getInstance(Settings::models().mSkyclouds.get(), mNextCloudMesh);
        mNextCloudUpdater = new CloudUpdater();
        mNextCloudUpdater->setOpacity(0.f);
        nextCloudMeshChild->addUpdateCallback(mNextCloudUpdater);
        mNextCloudMesh->setNodeMask(0);
        mNextCloudMesh->addChild(nextCloudMeshChild);

        mCloudNode->addChild(mCloudMesh);
        mCloudNode->addChild(mNextCloudMesh);

        ModVertexAlphaVisitor modClouds(ModVertexAlphaVisitor::Clouds);
        mCloudMesh->accept(modClouds);
        mNextCloudMesh->accept(modClouds);

        Shader::ShaderManager::DefineMap defines = {};
        Stereo::shaderStereoDefines(defines);
        auto program = mSceneManager->getShaderManager().getProgram("sky", defines);
        mEarlyRenderBinRoot->getOrCreateStateSet()->addUniform(new osg::Uniform("pass", -1));
        mEarlyRenderBinRoot->getOrCreateStateSet()->setAttributeAndModes(
            program, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);

        osg::ref_ptr<osg::Depth> depth = new SceneUtil::AutoDepth;
        depth->setWriteMask(false);
        mEarlyRenderBinRoot->getOrCreateStateSet()->setAttributeAndModes(depth);
        mEarlyRenderBinRoot->getOrCreateStateSet()->setMode(GL_BLEND, osg::StateAttribute::ON);

        mMoonScriptColor = Fallback::Map::getColour("Moons_Script_Color");

        mCreated = true;
    }

    SkyManager::~SkyManager()
    {
        if (mSkyRootNode)
        {
            mSkyRootNode->getParent(0)->removeChild(mSkyRootNode);
            mSkyRootNode = nullptr;
        }
    }

    int SkyManager::getMasserPhase() const
    {
        if (!mCreated)
            return 0;
        return mMasser->getPhaseInt();
    }

    int SkyManager::getSecundaPhase() const
    {
        if (!mCreated)
            return 0;
        return mSecunda->getPhaseInt();
    }

    bool SkyManager::isEnabled()
    {
        return mEnabled;
    }

    bool SkyManager::hasRain() const
    {
        return mPrecipitation->hasRain();
    }

    bool SkyManager::getRainRipplesEnabled() const
    {
        return mEnabled && mPrecipitation->ripplesEnabled();
    }

    float SkyManager::getPrecipitationAlpha() const
    {
        return mPrecipitation->getPrecipitationAlpha();
    }

    void SkyManager::update()
    {
        if (!mEnabled)
            return;

        // **The precipitation is not driven here.** It is one object both renderers read, and this
        // manager is one renderer's — so `RenderingManager` drives it, which is also the only place
        // that holds all three things a frame has to tell it. See `Weather::Conditions`.

        // **The deck's scroll and the stars' roll are turned by `RenderingManager` and handed here**,
        // because a ray tracer that draws its own sky has to turn the same one and this manager may
        // never have been created for it to ask.
        mNextCloudUpdater->setTextureCoord(mRoll.mClouds);
        mCloudUpdater->setTextureCoord(mRoll.mClouds);

        // morrowind rotates each cloud mesh independently
        osg::Quat rotation;
        rotation.makeRotate(Weather::defaultStormDirection(), mStormDirection);
        mCloudMesh->setAttitude(rotation);

        if (mNextCloudMesh->getNodeMask())
        {
            rotation.makeRotate(Weather::defaultStormDirection(), mNextStormDirection);
            mNextCloudMesh->setAttitude(rotation);
        }

        if (mAtmosphereNightNode->getNodeMask() != 0)
            mAtmosphereNightNode->setAttitude(osg::Quat(mRoll.mStars, osg::Vec3f(0, 0, 1)));
        mPrecipitationOccluder->update();
    }

    void SkyManager::setEnabled(bool enabled)
    {
        if (enabled && !mCreated)
            create();

        const osg::Node::NodeMask mask = enabled ? SceneUtil::Mask_Sky : 0u;

        mEarlyRenderBinRoot->setNodeMask(mask);
        mSkyNode->setNodeMask(mask);

        // A sky switched off drops nothing, and switching it back on has to build what it dropped —
        // which is what the dirty flag makes `setWeather` do rather than finding nothing changed.
        if (!enabled)
        {
            mPrecipitation->setWeather(Weather::Downpour{});
            mPrecipitationOccluder->disable();
            mDirtyParticlesEffect = true;
        }

        mEnabled = enabled;
    }

    void SkyManager::decoratePrecipitation()
    {
        // **What a rebuild threw away and only this renderer wants back.** `Precipitation` builds the
        // particle systems both renderers read and knows nothing about any of this: an occlusion
        // pass that keeps rain off the inside of a roof, a cull callback that freezes it under
        // water, and the two user values a generated pipeline reads off a drawable.
        const bool occluded = mPrecipitation->wantsOcclusion();

        for (osg::Node* node : { static_cast<osg::Node*>(mPrecipitation->getRainNode()),
                 static_cast<osg::Node*>(mPrecipitation->getEffectNode()) })
        {
            if (node == nullptr)
                continue;

            node->addCullCallback(mUnderwaterSwitch);

            SceneUtil::FindByClassVisitor findPSVisitor("ParticleSystem");
            node->accept(findPSVisitor);
            for (osg::Node* found : findPSVisitor.mFoundNodes)
            {
                found->setUserValue("simpleLighting", true);
                if (occluded)
                    found->setUserValue("particleOcclusion", true);
            }

            mSceneManager->recreateShaders(node);
        }

        if (!mPrecipitationOcclusion || !occluded)
        {
            mPrecipitationOccluder->disable();
            return;
        }

        mPrecipitationOccluder->enable();
        mPrecipitationOccluder->updateRange(mPrecipitation->getWrapRange());
    }

    void SkyManager::setMoonColour(bool red)
    {
        if (!mCreated)
            return;
        mSecunda->setColor(red ? mMoonScriptColor : osg::Vec4f(1, 1, 1, 1));
    }

    void SkyManager::setWeather(const WeatherResult& weather)
    {
        if (!mCreated)
            return;

        mIsStorm = weather.mDownpour.mIsStorm;
        if (mIsStorm)
            mStormDirection = weather.mStormDirection;

        // **The particles themselves are `MWRender::Precipitation`'s**, because a particle system is
        // a thing both renderers read and this one is only the rasterizer. What is left here is what
        // is genuinely the rasterizer's: the occlusion pass that keeps rain off the inside of a
        // roof, the cull callback that hides it under water, and the shader hints a generated
        // pipeline reads — none of which survive a rebuild, which is what the revision is for.
        const unsigned int was = mPrecipitation->getRevision();
        mPrecipitation->setWeather(weather.mDownpour);
        if (mPrecipitation->getRevision() != was || mDirtyParticlesEffect)
        {
            mDirtyParticlesEffect = false;
            decoratePrecipitation();
        }

        if (mClouds != weather.mCloudTexture)
        {
            mClouds = weather.mCloudTexture;

            const VFS::Path::Normalized texture
                = Misc::ResourceHelpers::correctTexturePath(VFS::Path::toNormalized(mClouds), *mSceneManager->getVFS());

            osg::ref_ptr<osg::Texture2D> cloudTex
                = new osg::Texture2D(mSceneManager->getImageManager()->getImage(texture));
            cloudTex->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
            cloudTex->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);

            mCloudUpdater->setTexture(std::move(cloudTex));
        }

        if (mStormDirection != weather.mStormDirection)
            mStormDirection = weather.mStormDirection;

        if (mNextStormDirection != weather.mNextStormDirection)
            mNextStormDirection = weather.mNextStormDirection;

        if (mNextClouds != weather.mNextCloudTexture)
        {
            mNextClouds = weather.mNextCloudTexture;

            if (!mNextClouds.empty())
            {
                const VFS::Path::Normalized texture = Misc::ResourceHelpers::correctTexturePath(
                    VFS::Path::toNormalized(mNextClouds), *mSceneManager->getVFS());

                osg::ref_ptr<osg::Texture2D> cloudTex
                    = new osg::Texture2D(mSceneManager->getImageManager()->getImage(texture));
                cloudTex->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
                cloudTex->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);

                mNextCloudUpdater->setTexture(std::move(cloudTex));
                mNextStormDirection = weather.mStormDirection;
            }
        }

        if (mCloudBlendFactor != weather.mCloudBlendFactor)
        {
            mCloudBlendFactor = std::clamp(weather.mCloudBlendFactor, 0.f, 1.f);

            mCloudUpdater->setOpacity(1.f - mCloudBlendFactor);
            mNextCloudUpdater->setOpacity(mCloudBlendFactor);
            mNextCloudMesh->setNodeMask(mCloudBlendFactor > 0.f ? ~0u : 0);
        }

        if (mCloudColour != weather.mFogColor)
        {
            // The lift is `Sky::cloudColour`'s, because the ray tracer lights the same deck with it.
            const osg::Vec4f lit = Sky::cloudColour(weather.mFogColor);

            mCloudUpdater->setEmissionColor(lit);
            mNextCloudUpdater->setEmissionColor(lit);

            mCloudColour = weather.mFogColor;
        }

        if (mSkyColour != weather.mSkyColor)
        {
            mSkyColour = weather.mSkyColor;

            mAtmosphereUpdater->setEmissionColor(mSkyColour);
            mMasser->setAtmosphereColor(mSkyColour);
            mSecunda->setAtmosphereColor(mSkyColour);
        }

        if (mFogColour != weather.mFogColor)
        {
            mFogColour = weather.mFogColor;
        }

        mMasser->adjustTransparency(weather.mGlareView);
        mSecunda->adjustTransparency(weather.mGlareView);

        mSun->setColor(weather.mSunDiscColor);
        mSun->adjustTransparency(weather.mGlareView * weather.mSunDiscColor.a());

        float nextStarsOpacity = weather.mNightFade * weather.mGlareView;

        if (weather.mNight && mStarsOpacity != nextStarsOpacity)
        {
            mStarsOpacity = nextStarsOpacity;

            mAtmosphereNightUpdater->setFade(mStarsOpacity);
        }

        mAtmosphereNightNode->setNodeMask(weather.mNight ? ~0u : 0);
    }

    float SkyManager::getBaseWindSpeed() const
    {
        if (!mCreated)
            return 0.f;

        return mBaseWindSpeed;
    }

    void SkyManager::setSunglare(bool enabled)
    {
        mSunglareEnabled = enabled;

        if (mSun)
            mSun->setSunglare(mSunglareEnabled);
    }

    void SkyManager::sunEnable()
    {
        if (!mCreated)
            return;

        mSun->setVisible(true);
    }

    void SkyManager::sunDisable()
    {
        if (!mCreated)
            return;

        mSun->setVisible(false);
    }

    void SkyManager::setSunDirection(const osg::Vec3f& direction)
    {
        if (!mCreated)
            return;

        mSun->setDirection(direction);
    }

    void SkyManager::setMasserState(const MoonState& state)
    {
        if (!mCreated)
            return;

        mMasser->setState(state);
    }

    void SkyManager::setSecundaState(const MoonState& state)
    {
        if (!mCreated)
            return;

        mSecunda->setState(state);
    }

    void SkyManager::setGlareTimeOfDayFade(float val)
    {
        mSun->setGlareTimeOfDayFade(val);
    }

    void SkyManager::setWaterHeight(float height)
    {
        mUnderwaterSwitch->setWaterLevel(height);
    }

    void SkyManager::listAssetsToPreload(
        std::vector<VFS::Path::Normalized>& models, std::vector<VFS::Path::Normalized>& textures)
    {
        models.push_back(Settings::models().mSkyatmosphere);
        if (mSceneManager->getVFS()->exists(Settings::models().mSkynight02.get()))
            models.push_back(Settings::models().mSkynight02);
        models.push_back(Settings::models().mSkynight01);
        models.push_back(Settings::models().mSkyclouds);

        models.push_back(Settings::models().mWeatherashcloud);
        models.push_back(Settings::models().mWeatherblightcloud);
        models.push_back(Settings::models().mWeathersnow);
        models.push_back(Settings::models().mWeatherblizzard);

        textures.emplace_back("textures/tx_mooncircle_full_s.dds");
        textures.emplace_back("textures/tx_mooncircle_full_m.dds");

        textures.emplace_back("textures/tx_masser_new.dds");
        textures.emplace_back("textures/tx_masser_one_wax.dds");
        textures.emplace_back("textures/tx_masser_half_wax.dds");
        textures.emplace_back("textures/tx_masser_three_wax.dds");
        textures.emplace_back("textures/tx_masser_one_wan.dds");
        textures.emplace_back("textures/tx_masser_half_wan.dds");
        textures.emplace_back("textures/tx_masser_three_wan.dds");
        textures.emplace_back("textures/tx_masser_full.dds");

        textures.emplace_back("textures/tx_secunda_new.dds");
        textures.emplace_back("textures/tx_secunda_one_wax.dds");
        textures.emplace_back("textures/tx_secunda_half_wax.dds");
        textures.emplace_back("textures/tx_secunda_three_wax.dds");
        textures.emplace_back("textures/tx_secunda_one_wan.dds");
        textures.emplace_back("textures/tx_secunda_half_wan.dds");
        textures.emplace_back("textures/tx_secunda_three_wan.dds");
        textures.emplace_back("textures/tx_secunda_full.dds");

        textures.emplace_back("textures/tx_sun_05.dds");
        textures.emplace_back("textures/tx_sun_flash_grey_05.dds");

        textures.emplace_back("textures/tx_raindrop_01.dds");
    }

    void SkyManager::setWaterEnabled(bool enabled)
    {
        mUnderwaterSwitch->setEnabled(enabled);
    }
}
