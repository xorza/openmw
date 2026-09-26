#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <components/settings/categories.hpp>
#include <components/vfs/pathutil.hpp>

namespace osg
{
    class Camera;
    class Group;
}

namespace osgViewer
{
    class Viewer;
}

namespace Debug
{
    struct DebugDrawer;
}

namespace Resource
{
    class ResourceSystem;
}

namespace SceneUtil
{
    class LightManager;
    class PerViewUniformStateUpdater;
    class ShadowManager;
    class SharedUniformStateUpdater;
    class StateUpdater;
}

namespace MWWorld
{
    class CellStore;
    class Ptr;
}

namespace MWRender
{
    class PostProcessor;
    class PrecipitationOccluder;
    class RenderingManager;
    struct SceneFrame;
    class SkyManager;
    class Water;

    /// The rasterizer's world: everything upstream's `RenderingManager` built for the frame that
    /// only the rasterizer reads, owned by the renderer that reads it.
    ///
    /// **Upstream's constructor, with the game's lines taken out.** The shadow technique, the
    /// shader defines, the three uniform updaters, the debug drawer, the sky dome, the
    /// post-processing chain, the water and the GL state on the roots and the camera are made here
    /// in the order `RenderingManager` made them, and fed each frame from `SceneFrame` where
    /// `RenderingManager` used to write into them directly. What the game keeps is what both
    /// renderers read.
    class GlWorld
    {
    public:
        GlWorld(osgViewer::Viewer& viewer, RenderingManager& world, osg::Group& worldRoot,
            SceneUtil::LightManager& sceneRoot, Resource::ResourceSystem& resources);
        ~GlWorld();

        PostProcessor& getPostProcessor() const { return *mPostProcessor; }

        /// What the frame says about the world, put where the rasterizer's objects read it. Before
        /// the update traversal, because most of them are update callbacks and upstream wrote them
        /// from `RenderingManager::update`, which ran before it too.
        void describe(const SceneFrame& frame);

        void addCell(const MWWorld::CellStore* cell);
        void removeCell(const MWWorld::CellStore* cell);
        void addWaterRippleEmitter(const MWWorld::Ptr& ptr);
        void removeWaterRippleEmitter(const MWWorld::Ptr& ptr);
        void emitWaterRipple(const osg::Vec3f& position);
        void clearRipples();

        void listAssetsToPreload(
            std::vector<VFS::Path::Normalized>& models, std::vector<VFS::Path::Normalized>& textures);

        /// The world is drawn, or the `tws` command has hidden it: what the water's reflection is
        /// told, since it draws the world again.
        void setWorldShown(bool shown);

        void processChangedSettings(const Settings::CategorySettingVector& changed);

        /// The wireframe toggle; the other modes are the game's or the renderer's.
        bool toggleWireframe();

    private:
        /// The GL state upstream put on the world root, the scene root and the camera, verbatim.
        void applyRootState(osg::Group& worldRoot, SceneUtil::LightManager& sceneRoot, osg::Camera& camera);

        /// The last description applied where applying has a side effect, so that it is applied on
        /// change: the shadow technique's mode is a rebuild.
        struct Applied
        {
            bool mOutdoors = false;
            bool mExterior = false;
            float mWaterHeight = 0.f;
            bool mWaterEnabled = false;
            /// Whether the water's height cull is in place: the terrain hands one out only once it
            /// has chunks, so an exterior asks again until it does.
            bool mWaterCulled = false;
            /// The occluder's `enable` adds a callback each call, so it follows the edge.
            bool mPrecipitating = false;
            bool mAny = false;
        };

        osgViewer::Viewer& mViewer;
        Resource::ResourceSystem& mResources;
        osg::ref_ptr<SceneUtil::LightManager> mSceneRoot;

        std::unique_ptr<SceneUtil::ShadowManager> mShadowManager;
        std::map<std::string, std::string> mAppliedShadowDefines;

        osg::ref_ptr<Debug::DebugDrawer> mDebugDraw;

        osg::ref_ptr<SceneUtil::StateUpdater> mStateUpdater;
        osg::ref_ptr<SceneUtil::SharedUniformStateUpdater> mSharedUniformStateUpdater;
        osg::ref_ptr<SceneUtil::PerViewUniformStateUpdater> mPerViewUniformStateUpdater;

        std::unique_ptr<SkyManager> mSky;
        bool mPrecipitationOcclusion = false;
        std::unique_ptr<PrecipitationOccluder> mPrecipitationOccluder;

        osg::ref_ptr<PostProcessor> mPostProcessor;

        std::unique_ptr<Water> mWater;

        Applied mApplied;
    };
}
