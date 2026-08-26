#pragma once

#include <osg/Node>
#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <components/vfs/pathutil.hpp>
#include <components/weather/downpour.hpp>

namespace osg
{
    class Camera;
    class Group;
    class Node;
    class PositionAttitudeTransform;
}

namespace osgParticle
{
    class BoxPlacer;
    class ParticleSystem;
}

namespace Resource
{
    class SceneManager;
}

namespace Weather
{
    class RainCounter;
    class RainShooter;

    /// Where the weather is happening, as the frame that is drawing it knows.
    ///
    /// **The three things a particle system cannot work out for itself, and one call so that no
    /// caller can supply only some of them.** They used to be three setters and an `update`, spread
    /// across `RenderingManager` and the rasterizer's sky manager — and the harness, driving the
    /// same class from a third place, set two of them and called neither of the others. The result
    /// was rain that never froze under water and storms that never turned to face the wind, in the
    /// one tool that exists to catch exactly that.
    struct Conditions
    {
        /// Where the eye is, which is what the box of drops slides along with.
        ///
        /// **Handed over rather than read off a camera.** A finite handful of drops is a whole
        /// rainstorm only because the box follows the player, and the eye is a thing the game keeps
        /// on `MWRender::Camera` and the harness keeps in a viewpoint — neither of them an
        /// `osg::Camera` this could have reached into and asked.
        osg::Vec3f mEye;

        /// Which way a storm drives what it carries, from `Weather::stormDirection`.
        ///
        /// **Per frame and not settled with the `Downpour`**, because an ash storm is aimed at
        /// whoever is watching and the game turns it as the transition runs.
        osg::Vec3f mStormDirection = defaultStormDirection();

        /// Whether the eye is under the water, which holds the drops where they are.
        ///
        /// **Told, and never worked out here.** Where the eye is relative to the water is a
        /// question the renderer already answers — `MWRender::Water::isUnderwater` in the game, a
        /// cell's water level in the harness — and it is on the frame before this is asked.
        /// Deriving it a second time from a water level of its own is how it came to be read off a
        /// cull traversal that the ray tracer never runs, and answered from the origin ever after.
        bool mUnderwater = false;
    };

    /// What the weather drops: rain, snow, and the clouds a storm drives past the eye.
    ///
    /// **Renderer-neutral, and in `components/` so that the harness can build one too.** Every one
    /// of these is an `osgParticle` system — the rain one built here and the rest loaded out of a
    /// NIF the weather names — and a particle system is a thing both renderers read. The rasterizer
    /// draws it as quads; the ray tracer finds the same systems in the graph and marches its sprite
    /// layer against them. There is one of each, built once, and neither renderer owns them.
    ///
    /// **It lived under `apps/openmw/` and that is why three bugs in it survived to the game.** The
    /// harness is how a rendering change is meant to be checked, and precipitation was the one part
    /// of the frame it could not build — so it was the one part where a wrong traversal mask, a
    /// gate read off a cull and an emitter nothing ever stepped all went unseen until somebody
    /// opened a window and stood in the rain.
    ///
    /// What is *not* here is everything about drawing them. The occlusion pass that keeps rain off
    /// the inside of a roof, the cull callback that hides the lot of it under water, the shader
    /// hints a generated pipeline reads — those are the rasterizer's and stay with it. `getRevision` is how
    /// it knows to put them back: the nodes below are torn down and rebuilt whenever the weather
    /// changes what falls, and whatever a renderer hung on them went with them.
    class Precipitation
    {
    public:
        /// @param parent where the nodes go. **Camera-relative in the rasterizer's graph**, because
        ///        the rain is a box that follows the eye rather than a place in the world.
        /// @param mask what the two subtrees are marked with, which is the rasterizer's business and
        ///        so comes from outside: a node mask names a pass, and `components/` has no passes.
        Precipitation(osg::Group* parent, Resource::SceneManager& scenes, osg::Node::NodeMask mask);
        ~Precipitation();

        /// What this weather drops, how hard, and how fast the wind drives it.
        void setWeather(const Downpour& weather);

        /// One frame's worth of driving: slides the box of drops to the eye, holds them still under
        /// water, and turns the storm's own effect to face where it is driving.
        ///
        /// **Every frame, by whoever is drawing.** Nothing under `getNode` moves on its own — the
        /// particles are stepped by whatever walks the graph, and this is the rest of it.
        void update(const Conditions& where);

        /// Everything it has built, for whoever is drawing.
        osg::Group* getNode() { return mNode.get(); }

        /// Bumped whenever what is under `getNode` is torn down and built again.
        ///
        /// **A renderer that decorated those nodes has to do it again**, because what it hung on
        /// them was thrown away with them. Nothing here knows what those decorations are.
        unsigned int getRevision() const { return mRevision; }

        /// The two subtrees, or null where this weather has neither. A renderer decorating them
        /// wants these rather than a walk of `getNode`, since only these two are its business.
        osg::Group* getRainNode() { return mRainNode.get(); }
        osg::PositionAttitudeTransform* getEffectNode() { return mParticleNode.get(); }

        bool hasRain() const { return mRainNode != nullptr; }

        /// Whether what is falling is the sort that dimples water. Rain and snow each carry their
        /// own answer in the content files and the rest of the weathers have none.
        bool ripplesEnabled() const;

        float getPrecipitationAlpha() const { return mPrecipitationAlpha; }
        float getBaseWindSpeed() const { return mBaseWindSpeed; }

        /// How far a particle travels before the wrap carries it back — the box an occlusion pass
        /// has to cover, which is why anything outside asks.
        osg::Vec3f getWrapRange() const;

        /// Whether what is falling is the sort worth occluding: rain, and Solstheim's snow. Ash and
        /// blight blow through a roof in the original too.
        bool wantsOcclusion() const;

    private:
        void createRain();
        void destroyRain();
        void updateRainParameters();

        Resource::SceneManager& mSceneManager;
        osg::Node::NodeMask mMask;

        /// Read by the wrap operator every step, so it is a member the operators hold by reference
        /// rather than a value they were built with.
        osg::Vec3f mEye;

        osg::ref_ptr<osg::Group> mNode;

        osg::ref_ptr<osg::Group> mRainNode;
        osg::ref_ptr<osgParticle::ParticleSystem> mRainParticleSystem;
        osg::ref_ptr<osgParticle::BoxPlacer> mPlacer;
        osg::ref_ptr<RainCounter> mCounter;
        osg::ref_ptr<RainShooter> mRainShooter;

        osg::ref_ptr<osg::PositionAttitudeTransform> mParticleNode;
        osg::ref_ptr<osg::Node> mParticleEffect;
        VFS::Path::Normalized mCurrentParticleEffect;

        std::string mRainEffect;
        float mRainSpeed = 0.f;
        float mRainDiameter = 0.f;
        float mRainMinHeight = 0.f;
        float mRainMaxHeight = 0.f;
        float mRainEntranceSpeed = 1.f;
        int mRainMaxRaindrops = 0;

        bool mRainRipplesEnabled;
        bool mSnowRipplesEnabled;

        float mPrecipitationAlpha = 0.f;
        float mWindSpeed = 0.f;
        float mBaseWindSpeed = 0.f;

        bool mIsStorm = false;
        osg::Vec3f mStormDirection = defaultStormDirection();

        bool mUnderwater = false;

        unsigned int mRevision = 0;
    };
}
