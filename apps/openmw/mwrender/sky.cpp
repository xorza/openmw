#include "sky.hpp"

#include <osg/Depth>
#include <osg/PositionAttitudeTransform>

#include <components/settings/values.hpp>

#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/material.hpp>
#include <components/sceneutil/rtt.hpp>
#include <components/sceneutil/shadow.hpp>

#include <components/resource/imagemanager.hpp>
#include <components/resource/scenemanager.hpp>

#include <components/vfs/manager.hpp>

#include <components/misc/resourcehelpers.hpp>
#include <components/stereo/stereomanager.hpp>

#include <components/nifosg/particle.hpp>

#include <components/sky/skyclock.hpp>

#include "../mwworld/datetimemanager.hpp"
#include "../mwworld/weather.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"

#include "renderbin.hpp"
#include "skyutil.hpp"
#include "util.hpp"
#include "vismask.hpp"

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
            camera->setNodeMask(MWRender::Mask_RenderToTexture);
            camera->setCullMask(MWRender::Mask_Sky);
            camera->addChild(mEarlyRenderBinRoot);
            SceneUtil::ShadowManager::instance().disableShadowsForStateSet(*camera->getOrCreateStateSet());
        }

    private:
        osg::ref_ptr<osg::Group> mEarlyRenderBinRoot;
    };

}

namespace MWRender
{
    SkyManager::SkyManager(osg::Group* parentNode, Resource::SceneManager* sceneManager, bool enableSkyRTT)
        : mSceneManager(sceneManager)
        , mCreated(false)
        , mIsStorm(false)
        , mTimescaleClouds(Fallback::Map::getBool("Weather_Timescale_Clouds"))
        , mCloudAnimationTimer(0.f)
        , mStormDirection(MWWorld::Weather::defaultDirection())
        , mClouds()
        , mNextClouds()
        , mCloudBlendFactor(0.f)
        , mCloudSpeed(0.f)
        , mStarsOpacity(0.f)
        , mBaseWindSpeed(0.f)
        , mEnabled(true)
        , mSunglareEnabled(true)
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
        mSkyNode->setNodeMask(Mask_Sky);
        mSkyNode->addChild(mEarlyRenderBinRoot);
        mSkyRootNode->addChild(mSkyNode);
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

    void SkyManager::update(float duration)
    {
        if (!mEnabled)
            return;

        const MWWorld::DateTimeManager& timeManager = *MWBase::Environment::get().getWorld()->getTimeManager();

        // UV Scroll the clouds
        float cloudDelta = duration * mCloudSpeed / 400.f;
        if (mTimescaleClouds)
            cloudDelta *= timeManager.getGameTimeScale() / 60.f;

        mCloudAnimationTimer += cloudDelta;
        if (mCloudAnimationTimer >= 4.f)
            mCloudAnimationTimer -= 4.f;

        mNextCloudUpdater->setTextureCoord(mCloudAnimationTimer);
        mCloudUpdater->setTextureCoord(mCloudAnimationTimer);

        // morrowind rotates each cloud mesh independently
        osg::Quat rotation;
        rotation.makeRotate(MWWorld::Weather::defaultDirection(), mStormDirection);
        mCloudMesh->setAttitude(rotation);

        if (mNextCloudMesh->getNodeMask())
        {
            rotation.makeRotate(MWWorld::Weather::defaultDirection(), mNextStormDirection);
            mNextCloudMesh->setAttitude(rotation);
        }

        if (mAtmosphereNightNode->getNodeMask() != 0)
            mAtmosphereNightNode->setAttitude(osg::Quat(Sky::starRoll(timeManager.getGameTime()), osg::Vec3f(0, 0, 1)));
    }

    void SkyManager::setEnabled(bool enabled)
    {
        if (enabled && !mCreated)
            create();

        const osg::Node::NodeMask mask = enabled ? Mask_Sky : 0u;

        mEarlyRenderBinRoot->setNodeMask(mask);
        mSkyNode->setNodeMask(mask);

        mEnabled = enabled;
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

        mBaseWindSpeed = weather.mBaseWindSpeed;

        mIsStorm = weather.mIsStorm;

        if (mIsStorm)
            mStormDirection = weather.mStormDirection;

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
            osg::Vec4f clr(weather.mFogColor);
            clr += osg::Vec4f(0.13f, 0.13f, 0.13f, 0.f);

            mCloudUpdater->setEmissionColor(clr);
            mNextCloudUpdater->setEmissionColor(clr);

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

        mCloudSpeed = weather.mCloudSpeed;

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

    void SkyManager::listAssetsToPreload(
        std::vector<VFS::Path::Normalized>& models, std::vector<VFS::Path::Normalized>& textures)
    {
        models.push_back(Settings::models().mSkyatmosphere);
        if (mSceneManager->getVFS()->exists(Settings::models().mSkynight02.get()))
            models.push_back(Settings::models().mSkynight02);
        models.push_back(Settings::models().mSkynight01);
        models.push_back(Settings::models().mSkyclouds);

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
    }
}
