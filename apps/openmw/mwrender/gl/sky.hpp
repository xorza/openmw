#ifndef OPENMW_MWRENDER_SKY_H
#define OPENMW_MWRENDER_SKY_H

#include <memory>
#include <string>
#include <vector>

#include <osg/Vec4f>
#include <osg/ref_ptr>

#include <components/sky/clouds.hpp>
#include <components/sky/skyroll.hpp>
#include <components/vfs/pathutil.hpp>

#include "precipitationocclusion.hpp"
#include "skyutil.hpp"

namespace osg
{
    class Group;
    class Node;
    class PositionAttitudeTransform;
    class Camera;
}

namespace osgParticle
{
    class ParticleSystem;
    class BoxPlacer;
}

namespace Resource
{
    class SceneManager;
}

namespace SceneUtil
{
    class RTTNode;
    class Material;
}

namespace Weather
{
    class Precipitation;
}

namespace MWRender
{

    ///@brief The SkyManager handles rendering of the sky domes, celestial bodies as well as other objects that need to
    /// be rendered
    /// relative to the camera (e.g. weather particle effects)
    class SkyManager
    {
    public:
        SkyManager(osg::Group* parentNode, osg::Group* rootNode, osg::Camera* camera,
            Resource::SceneManager* sceneManager, bool enableSkyRTT);
        ~SkyManager();

        void update();

        void setEnabled(bool enabled);

        int getMasserPhase() const;
        ///< 0 new moon, 1 waxing or waning cresecent, 2 waxing or waning half,
        /// 3 waxing or waning gibbous, 4 full moon

        int getSecundaPhase() const;
        ///< 0 new moon, 1 waxing or waning cresecent, 2 waxing or waning half,
        /// 3 waxing or waning gibbous, 4 full moon

        void setMoonColour(bool red);
        ///< change Secunda colour to red

        void setWeather(const WeatherResult& weather);

        void sunEnable();

        void sunDisable();

        bool isEnabled();

        bool hasRain() const;

        bool getRainRipplesEnabled() const;

        float getPrecipitationAlpha() const;

        void setSunDirection(const osg::Vec3f& direction);

        void setMasserState(const MoonState& state);
        void setSecundaState(const MoonState& state);

        void setGlareTimeOfDayFade(float val);

        /// Enable or disable the water plane (used to remove underwater weather particles)
        void setWaterEnabled(bool enabled);

        /// Set height of water plane (used to remove underwater weather particles)
        void setWaterHeight(float height);

        void listAssetsToPreload(
            std::vector<VFS::Path::Normalized>& models, std::vector<VFS::Path::Normalized>& textures);

        float getBaseWindSpeed() const;

        /// What the weather drops. **Built here and drawn by whoever is drawing**, because a particle
        /// system is not a rasterizer's or a tracer's — it is the world's, and there is one of it.
        Weather::Precipitation* getPrecipitation() { return mPrecipitation.get(); }

        /// How far the clouds have scrolled and the stars have rolled, which `MWRender::RenderingManager`
        /// turns and hands down. **Not advanced here**: this manager belongs to one of the two
        /// renderers and is built lazily, so a clock inside it is one the other cannot read.
        void setRoll(const Sky::SkyRoll& roll) { mRoll = roll; }

        void setSunglare(bool enabled);

        SceneUtil::RTTNode* getSkyRTT() { return mSkyRTT.get(); }

        osg::Vec4f getSkyColor() const { return mSkyColour; }

    private:
        void create();
        ///< no need to call this, automatically done on first enable()

        Resource::SceneManager* mSceneManager;

        osg::Camera* mCamera;

        osg::ref_ptr<CameraRelativeTransform> mSkyRootNode;
        osg::ref_ptr<osg::Group> mSkyNode;
        osg::ref_ptr<osg::Group> mEarlyRenderBinRoot;

        osg::ref_ptr<UnderwaterSwitchCallback> mUnderwaterSwitch;

        osg::ref_ptr<osg::Group> mCloudNode;

        osg::ref_ptr<CloudUpdater> mCloudUpdater;
        osg::ref_ptr<CloudUpdater> mNextCloudUpdater;
        osg::ref_ptr<osg::PositionAttitudeTransform> mCloudMesh;
        osg::ref_ptr<osg::PositionAttitudeTransform> mNextCloudMesh;

        osg::ref_ptr<osg::Node> mAtmosphereDay;

        osg::ref_ptr<osg::PositionAttitudeTransform> mAtmosphereNightNode;
        osg::ref_ptr<AtmosphereNightUpdater> mAtmosphereNightUpdater;

        osg::ref_ptr<AtmosphereUpdater> mAtmosphereUpdater;

        std::unique_ptr<Sun> mSun;
        std::unique_ptr<Moon> mMasser;
        std::unique_ptr<Moon> mSecunda;

        bool mPrecipitationOcclusion = false;
        std::unique_ptr<PrecipitationOccluder> mPrecipitationOccluder;

        bool mCreated;

        bool mIsStorm;

        /// How far the deck has scrolled and the stars have turned, as handed down.
        Sky::SkyRoll mRoll;

        // particle system rotation is independent of cloud rotation internally
        osg::Vec3f mStormDirection;
        osg::Vec3f mNextStormDirection;

        // remember some settings so we don't have to apply them again if they didn't change
        std::string mClouds;
        std::string mNextClouds;
        float mCloudBlendFactor;
        float mStarsOpacity;
        osg::Vec4f mCloudColour;
        osg::Vec4f mSkyColour;
        osg::Vec4f mFogColour;

        float mWindSpeed;
        float mBaseWindSpeed;

        bool mEnabled;
        bool mSunglareEnabled;

        float mPrecipitationAlpha;
        bool mDirtyParticlesEffect;

        /// What falls out of the weather, which both renderers draw and neither owns.
        std::unique_ptr<Weather::Precipitation> mPrecipitation;

        /// Hangs this renderer's own concerns back on whatever `mPrecipitation` has just built: the
        /// occlusion pass, the underwater cull callback and the generated shaders' hints.
        void decoratePrecipitation();

        osg::Vec4f mMoonScriptColor;

        osg::ref_ptr<SceneUtil::RTTNode> mSkyRTT;
    };
}

#endif
