#ifndef OPENMW_MWRENDER_WEATHERRESULT_H
#define OPENMW_MWRENDER_WEATHERRESULT_H

#include <string>

#include <osg/Vec4f>

#include <components/esm/refid.hpp>
#include <components/sky/moonstate.hpp>

namespace MWRender
{
    /// Where a moon stands, under the name the game spells it by. `Sky::` because both renderers
    /// read it off the frame; `MWRender::` because the weather manager and its tests always wrote it.
    using MoonState = Sky::MoonState;

    /// What the weather system worked out this moment is, for whatever draws the sky.
    ///
    /// **Here rather than beside the dome that reads it.** These are the weather's own numbers —
    /// colours, speeds, the textures it names — computed by `MWWorld::WeatherManager` and handed
    /// down, so the world's weather system names no renderer.
    struct WeatherResult
    {
        std::string mCloudTexture;
        std::string mNextCloudTexture;
        float mCloudBlendFactor;

        osg::Vec4f mFogColor;

        osg::Vec4f mAmbientColor;

        osg::Vec4f mSkyColor;

        // sun light color
        osg::Vec4f mSunColor;

        // alpha is the sun transparency
        osg::Vec4f mSunDiscColor;

        float mFogDepth;

        float mDLFogFactor;
        float mDLFogOffset;

        float mWindSpeed;
        float mBaseWindSpeed;
        float mCurrentWindSpeed;
        float mNextWindSpeed;

        float mCloudSpeed;

        float mGlareView;

        bool mNight; // use night skybox
        float mNightFade; // fading factor for night skybox

        bool mIsStorm;

        ESM::RefId mAmbientLoopSoundID;
        ESM::RefId mRainLoopSoundID;
        float mAmbientSoundVolume;

        std::string mParticleEffect;
        std::string mRainEffect;
        float mPrecipitationAlpha;

        float mRainDiameter;
        float mRainMinHeight;
        float mRainMaxHeight;
        float mRainSpeed;
        float mRainEntranceSpeed;
        int mRainMaxRaindrops;

        osg::Vec3f mStormDirection;
        osg::Vec3f mNextStormDirection;
    };
}

#endif
