#ifndef GAME_RENDER_FRAMEDESCRIBER_H
#define GAME_RENDER_FRAMEDESCRIBER_H

#include <optional>

#include <osg/Matrixf>
#include <osg/Vec3f>
#include <osg/Vec4f>

#include "sceneframe.hpp"

namespace osg
{
    class FrameStamp;
    class Node;
}

namespace SceneUtil
{
    class Light;
}

namespace Terrain
{
    class ObjectStorage;
    class World;
}

namespace MWRender
{
    class FogManager;
    class Precipitation;

    /// What a frame's description reads off the objects upstream's `RenderingManager` owns, handed
    /// over at the moment the frame is described rather than kept: they are made in that class's
    /// constructor body and the terrain changes with the worldspace.
    struct FrameSources
    {
        osg::Node& mScene;
        const osg::FrameStamp& mWhen;

        /// The sun light as the four setters left it: the one copy of what they wrote.
        const SceneUtil::Light& mSun;

        /// The ambient before `updateAmbient` added the Night-Eye lift, so the frame can say how
        /// much it added.
        osg::Vec4f mAmbientBeforeNightEye;

        const FogManager& mFog;

        /// Where the eye stands, for the water's answer.
        osg::Vec3f mEyePosition;

        const Precipitation& mPrecipitation;
        Terrain::World& mTerrain;
        const Terrain::ObjectStorage& mObjectStorage;
        EyeState mEye;
    };

    /// What the game decides about a frame beyond what upstream's objects already hold, and the
    /// frame itself. Every fact this fork added beside `RenderingManager` lives here, so that class
    /// keeps one member for them and `renderingmanager.cpp` reads as upstream's with the seam edits.
    class FrameDescriber
    {
    public:
        WaterState& getWater() { return mWater; }
        const WaterState& getWater() const { return mWater; }

        /// `RenderingManager::setSunColour`'s third argument: the one number the light does not hold.
        void setSunVisibility(float visibility) { mSunVisibility = visibility; }

        /// What `RenderingManager::setSkyEnabled` was last told.
        void setSkyShown(bool shown) { mSkyShown = shown; }

        /// `RenderingManager::skySetMoonColour`, and off again for a new game.
        void setMoonRed(bool red) { mMoonRed = red; }

        /// What `updateProjectionMatrix` settled on: the reversed-depth form where the depth buffer
        /// is reversed, which is what a shader reads.
        void setProjection(const osg::Matrixf& projection) { mProjection = projection; }
        const osg::Matrixf& getProjection() const { return mProjection; }

        /// What `RenderingManager::update` was handed, for the frame that follows it.
        void setStep(float deltaTime, bool paused)
        {
            mDeltaTime = deltaTime;
            mPaused = paused;
        }

        /// Describes this frame off `sources` and the facts kept here, and keeps it until the next.
        const SceneFrame& describe(const FrameSources& sources);

        /// The frame `describe` last made. Asserts that it has.
        const SceneFrame& get() const;

    private:
        /// See `WorldState` in sceneframe.hpp.
        WorldState describeWorld(const FrameSources& sources) const;

        WaterState mWater;
        float mSunVisibility = 0.f;
        bool mSkyShown = false;
        bool mMoonRed = false;
        osg::Matrixf mProjection;
        float mDeltaTime = 0.f;
        bool mPaused = false;

        /// This frame, from `describe` to the next, and the two records it refers to; empty before
        /// the first.
        WorldState mWorld;
        EyeState mEye;
        std::optional<SceneFrame> mFrame;
    };
}

#endif
