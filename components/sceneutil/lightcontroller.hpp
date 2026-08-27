#ifndef OPENMW_COMPONENTS_SCENEUTIL_LIGHTCONTROLLER_H
#define OPENMW_COMPONENTS_SCENEUTIL_LIGHTCONTROLLER_H

namespace SceneUtil
{

    /// @brief How much of what a light radiates is arriving this frame: a flicker, a pulse, or the
    /// steady one.
    ///
    /// **The animation and nothing else.** What a light is made of, and what dims it, belong to the
    /// LightSource that owns one of these — so a light nobody thought to animate still follows the
    /// actor carrying it, and a colour the animation has no opinion about still reaches the frame.
    class LightController
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

        void setType(LightType type) { mType = type; }

        /// The multiplier for one frame, with the animation advanced to it.
        /// @param simulationTime the frame stamp's, in seconds.
        float advance(double simulationTime);

    private:
        LightType mType;
        float mPhase;
        float mBrightness;
        double mStartTime;
        double mLastTime;
        float mTicksToAdvance;
    };

}

#endif
