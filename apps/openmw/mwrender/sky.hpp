#ifndef OPENMW_MWRENDER_SKY_H
#define OPENMW_MWRENDER_SKY_H

#include <memory>
#include <string>
#include <vector>

#include <osg/Vec4f>
#include <osg/ref_ptr>

#include <components/vfs/pathutil.hpp>

#include "skyutil.hpp"

namespace osg
{
    class Group;
    class Node;
    class PositionAttitudeTransform;
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

namespace MWRender
{
    ///@brief The SkyManager handles rendering of the sky domes and celestial bodies
    class SkyManager
    {
    public:
        SkyManager(osg::Group* parentNode, Resource::SceneManager* sceneManager, bool enableSkyRTT);
        ~SkyManager();

        void update(float duration);

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

        void setSunDirection(const osg::Vec3f& direction);

        void setMasserState(const MoonState& state);
        void setSecundaState(const MoonState& state);

        void setGlareTimeOfDayFade(float val);

        void listAssetsToPreload(
            std::vector<VFS::Path::Normalized>& models, std::vector<VFS::Path::Normalized>& textures);

        float getBaseWindSpeed() const;

        void setSunglare(bool enabled);

        SceneUtil::RTTNode* getSkyRTT() { return mSkyRTT.get(); }

        osg::Vec4f getSkyColor() const { return mSkyColour; }

    private:
        void create();
        ///< no need to call this, automatically done on first enable()

        Resource::SceneManager* mSceneManager;

        osg::ref_ptr<CameraRelativeTransform> mSkyRootNode;
        osg::ref_ptr<osg::Group> mSkyNode;
        osg::ref_ptr<osg::Group> mEarlyRenderBinRoot;

        osg::ref_ptr<osg::Group> mCloudNode;

        osg::ref_ptr<CloudUpdater> mCloudUpdater;
        osg::ref_ptr<CloudUpdater> mNextCloudUpdater;
        osg::ref_ptr<osg::PositionAttitudeTransform> mCloudMesh;
        osg::ref_ptr<osg::PositionAttitudeTransform> mNextCloudMesh;

        osg::ref_ptr<osg::Node> mAtmosphereDay;

        osg::ref_ptr<osg::PositionAttitudeTransform> mAtmosphereNightNode;
        float mAtmosphereNightRoll;
        osg::ref_ptr<AtmosphereNightUpdater> mAtmosphereNightUpdater;

        osg::ref_ptr<AtmosphereUpdater> mAtmosphereUpdater;

        std::unique_ptr<Sun> mSun;
        std::unique_ptr<Moon> mMasser;
        std::unique_ptr<Moon> mSecunda;

        bool mCreated;

        bool mIsStorm;

        bool mTimescaleClouds;
        float mCloudAnimationTimer;

        osg::Vec3f mStormDirection;
        osg::Vec3f mNextStormDirection;

        // remember some settings so we don't have to apply them again if they didn't change
        std::string mClouds;
        std::string mNextClouds;
        float mCloudBlendFactor;
        float mCloudSpeed;
        float mStarsOpacity;
        osg::Vec4f mCloudColour;
        osg::Vec4f mSkyColour;
        osg::Vec4f mFogColour;

        float mBaseWindSpeed;

        bool mEnabled;
        bool mSunglareEnabled;

        osg::Vec4f mMoonScriptColor;

        osg::ref_ptr<SceneUtil::RTTNode> mSkyRTT;
    };
}

#endif
