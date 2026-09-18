#ifndef GAME_RENDER_PRECIPITATION_H
#define GAME_RENDER_PRECIPITATION_H

#include <string>
#include <vector>

#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <components/vfs/pathutil.hpp>

#include "skystate.hpp"

namespace osg
{
    class Camera;
    class Group;
    class Node;
    class PositionAttitudeTransform;
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

namespace MWRender
{
    /// What the weather drops: the rain box and the driven effect — snow, ash, blight — as particle
    /// systems in the scene graph, camera-relative, frozen under water. Upstream's, out of
    /// `SkyManager`: both renderers draw these particles, and only one draws the dome, so they hang
    /// under a root of their own and the dome keeps its own.
    class Precipitation
    {
    public:
        Precipitation(osg::Group* parentNode, osg::Camera* camera, Resource::SceneManager* sceneManager);
        ~Precipitation();

        void update();

        void setEnabled(bool enabled);

        /// The weather and the storm's direction, as the weather manager settled them.
        void setWeather(const SkyState& sky);

        bool hasRain() const;

        bool getRainRipplesEnabled() const;

        float getPrecipitationAlpha() const;

        /// Enable or disable the water plane (used to remove underwater weather particles)
        void setWaterEnabled(bool enabled);

        /// Set height of water plane (used to remove underwater weather particles)
        void setWaterHeight(float height);

        /// The rain box and the driven effect, or null where there is none — and nothing falls
        /// while the sky is off: the root's mask hides both nodes from the rasterizer's cull the
        /// moment the player steps indoors, and a walk that starts at the node never meets that
        /// mask. Both are camera-relative.
        osg::Group* getRainNode() const { return mEnabled ? mRainNode.get() : nullptr; }
        osg::PositionAttitudeTransform* getParticleNode() const { return mEnabled ? mParticleNode.get() : nullptr; }

        /// The root everything here hangs under, for a renderer that wants state on it.
        osg::Group& getRoot() { return *mRoot; }

        /// What the cull traversal tells the root; a renderer that culls nothing says it here
        void setViewPoint(const osg::Vec3f& eye) { mRoot->setLastViewPoint(eye); }

        void listAssetsToPreload(
            std::vector<VFS::Path::Normalized>& models, std::vector<VFS::Path::Normalized>& textures);

        /// Whether the rasterizer's occluder keeps what is falling out from under roofs, and over
        /// what range. The state upstream's calls on the occluder left it in — enabled where the
        /// rain was made, disabled where it was destroyed or the effect went, the range whichever
        /// was set last — and not a function of what is falling, because those calls were not one:
        /// rain that stops under snow leaves the snow unoccluded until the effect changes.
        bool isOccluded() const { return mOccluded; }
        const osg::Vec3f& getOcclusionRange() const { return mOcclusionRange; }

    private:
        void createRain();
        void destroyRain();
        void switchUnderwaterRain();
        void updateRainParameters();

        Resource::SceneManager* mSceneManager;

        osg::Camera* mCamera;

        osg::ref_ptr<CameraRelativeTransform> mRoot;

        osg::ref_ptr<osg::PositionAttitudeTransform> mParticleNode;
        osg::ref_ptr<osg::Node> mParticleEffect;
        osg::ref_ptr<UnderwaterSwitchCallback> mUnderwaterSwitch;

        osg::ref_ptr<osg::Group> mRainNode;
        osg::ref_ptr<osgParticle::ParticleSystem> mRainParticleSystem;
        osg::ref_ptr<osgParticle::BoxPlacer> mPlacer;
        osg::ref_ptr<RainCounter> mCounter;
        osg::ref_ptr<RainShooter> mRainShooter;

        bool mIsStorm;

        // particle system rotation is independent of cloud rotation internally
        osg::Vec3f mStormParticleDirection;

        VFS::Path::Normalized mCurrentParticleEffect;

        std::string mRainEffect;
        float mRainSpeed;
        float mRainDiameter;
        float mRainMinHeight;
        float mRainMaxHeight;
        float mRainEntranceSpeed;
        int mRainMaxRaindrops;
        bool mRainRipplesEnabled;
        bool mSnowRipplesEnabled;
        float mWindSpeed;

        bool mEnabled;

        float mPrecipitationAlpha;
        bool mDirtyParticlesEffect;

        bool mOccluded = false;
        osg::Vec3f mOcclusionRange;
    };
}

#endif
