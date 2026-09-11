#ifndef OPENMW_MWRENDER_WEATHERRESULT_H
#define OPENMW_MWRENDER_WEATHERRESULT_H

#include <string>

#include <osg/Vec4f>

#include <components/esm/refid.hpp>
#include <components/weather/downpour.hpp>

namespace MWRender
{
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

        /// The two the transition works in, which are neither weather's settled answer: what each
        /// side of it is blowing at right now. `mDownpour.mWindSpeed` is what they mix to.
        float mCurrentWindSpeed;
        float mNextWindSpeed;

        float mCloudSpeed;

        float mGlareView;

        bool mNight; // use night skybox
        float mNightFade; // fading factor for night skybox

        ESM::RefId mAmbientLoopSoundID;
        ESM::RefId mRainLoopSoundID;
        float mAmbientSoundVolume;

        /// Everything that falls, handed to `Weather::Precipitation` unchanged: one struct, so the
        /// rule that turns a base wind into a gust is applied in one place.
        ::Weather::Downpour mDownpour;

        osg::Vec3f mStormDirection;
        osg::Vec3f mNextStormDirection;
    };
}

#endif
