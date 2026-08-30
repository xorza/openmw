#ifndef OPENMW_COMPONENTS_SCENEUTIL_LIGHTCONTROLLER_H
#define OPENMW_COMPONENTS_SCENEUTIL_LIGHTCONTROLLER_H

#include <components/sceneutil/nodecallback.hpp>
#include <osg/Vec4f>

namespace SceneUtil
{

    class LightSource;

    /// @brief Controller class to handle a pulsing and/or flickering light
    class LightController : public SceneUtil::NodeCallback<LightController, SceneUtil::LightSource*>
    {
    public:
        enum LightType
        {
            LT_Normal,
            LT_Flicker,
            LT_FlickerSlow,
            LT_Pulse,
            LT_PulseSlow
        };

        LightController();

        void setType(LightType type);

        void setDiffuse(const osg::Vec4f& color);
        void setSpecular(const osg::Vec4f& color);

        void operator()(SceneUtil::LightSource* node, osg::NodeVisitor* nv);

        /// What the record's flags said this light does.
        LightType getType() const { return mType; }

        /// What the light radiates before the animation dims it. The colours inside the light itself
        /// are one frame's: this writes them from these every frame, and LightManager's distance
        /// fade then scales in place what it finds there.
        const osg::Vec4f& getDiffuse() const { return mDiffuseColor; }

    private:
        LightType mType;
        osg::Vec4f mDiffuseColor;
        osg::Vec4f mSpecularColor;
        float mPhase;
        float mBrightness;
        double mStartTime;
        double mLastTime;
        float mTicksToAdvance;
    };

}

#endif
