#ifndef OPENMW_MWRENDER_RENDERINGMANAGER_H
#define OPENMW_MWRENDER_RENDERINGMANAGER_H

#include "framedescriber.hpp"
#include "ground.hpp"
#include "objects.hpp"
#include "objectstorage.hpp"
#include "renderinginterface.hpp"
#include "rendermode.hpp"

#include <components/settings/settings.hpp>
#include <components/vfs/pathutil.hpp>

#include <osg/ref_ptr>

#include <osgUtil/IncrementalCompileOperation>

#include <deque>
#include <map>
#include <memory>
#include <span>
#include <unordered_map>

namespace osg
{
    class Group;
    class PositionAttitudeTransform;
}

namespace osgUtil
{
    class IntersectionVisitor;
    class Intersector;
}

namespace Resource
{
    class ResourceSystem;
}

namespace osgViewer
{
    class Viewer;
}

namespace ESM
{
    struct Cell;
    struct FormId;
    using RefNum = FormId;
}

namespace Terrain
{
    class World;
}

namespace Fallback
{
    class Map;
}

namespace SceneUtil
{
    class WorkQueue;
    class UnrefQueue;
    class Light;
}

namespace DetourNavigator
{
    struct Navigator;
    struct Settings;
    struct AgentBounds;
}

namespace MWWorld
{
    class GroundcoverStore;
    class Cell;
}

namespace MWRender
{
    class IntersectionVisitorWithIgnoreList;

    class EffectManager;
    class ScreenshotManager;
    class FogManager;
    class Precipitation;
    class NpcAnimation;
    class Pathgrid;
    class Camera;
    class TerrainStorage;
    class LandManager;
    class NavMesh;
    class ActorsPaths;
    class RecastMesh;
    class ObjectPaging;
    class Groundcover;
    class PostProcessor;
    class Renderer;

    class RenderingManager : public MWRender::RenderingInterface
    {
    public:
        RenderingManager(Renderer& renderer, osg::ref_ptr<osg::Group> rootNode,
            Resource::ResourceSystem* resourceSystem, SceneUtil::WorkQueue* workQueue,
            DetourNavigator::Navigator& navigator, const MWWorld::GroundcoverStore& groundcoverStore,
            SceneUtil::UnrefQueue& unrefQueue);
        ~RenderingManager();

        osgUtil::IncrementalCompileOperation* getIncrementalCompileOperation();

        MWRender::Objects& getObjects() override;

        Resource::ResourceSystem* getResourceSystem();

        SceneUtil::WorkQueue* getWorkQueue();
        Terrain::World* getTerrain();

        void preloadCommonAssets();

        double getReferenceTime() const;

        osg::Group* getSceneRoot();

        /// The renderer this draws through, for what the game asks a renderer directly: its shaders
        /// and its scripting.
        Renderer& getRenderer() { return mRenderer; }

        /// The sun's light as the game keeps it: colours, position, what the weather settled on.
        /// The rasterizer lights through it, the ray tracer reads it off the frame.
        SceneUtil::Light& getSunLight() { return *mSunLight; }

        /// The node the rain and the weather effect hang under, for a renderer that wants state on
        /// it: the rasterizer's shadow and normals exclusions, and its occluder's depth map.
        osg::Group& getPrecipitationRoot();

        void setNightEyeFactor(float factor);

        void setAmbientColour(const osg::Vec4f& colour);

        int skyGetMasserPhase() const;
        int skyGetSecundaPhase() const;
        void skySetMoonColour(bool red);

        const osg::Vec4f& getSunLightPosition() const;
        void setSunDirection(const osg::Vec3f& direction);
        void setSunColour(const osg::Vec4f& diffuse, const osg::Vec4f& specular, float sunVis);

        void configureAmbient(const MWWorld::Cell& cell);
        void configureFog(const MWWorld::Cell& cell);
        void configureFog(
            float fogDepth, float underwaterFog, float dlFactor, float dlOffset, const osg::Vec4f& colour);

        void addCell(const MWWorld::CellStore* store);
        void removeCell(const MWWorld::CellStore* store);

        void enableTerrain(bool enable, ESM::RefId worldspace);

        void updatePtr(const MWWorld::Ptr& old, const MWWorld::Ptr& updated);

        void rotateObject(const MWWorld::Ptr& ptr, const osg::Quat& rot);
        void moveObject(const MWWorld::Ptr& ptr, const osg::Vec3f& pos);
        void scaleObject(const MWWorld::Ptr& ptr, const osg::Vec3f& scale);

        void removeObject(const MWWorld::Ptr& ptr);

        void setWaterEnabled(bool enabled);
        void setWaterHeight(float level);

        /// Take a screenshot of w*h onto the given image, not including the GUI.
        void screenshot(osg::Image* image, int w, int h);

        struct RayResult
        {
            bool mHit;
            osg::Vec3f mHitNormalWorld;
            osg::Vec3f mHitPointWorld;
            MWWorld::Ptr mHitObject;
            ESM::RefNum mHitRefnum;
            float mRatio;
        };

        RayResult castRay(const osg::Vec3f& origin, const osg::Vec3f& dest, bool ignorePlayer,
            bool ignoreActors = false, bool ignoreTerrain = false, std::span<const MWWorld::Ptr> ignoreList = {});

        /// Return the object under the mouse cursor / crosshair position, given by nX and nY normalized screen
        /// coordinates, where (0,0) is the top left corner.
        RayResult castCameraToViewportRay(const float nX, const float nY, float maxDistance, bool ignorePlayer,
            bool ignoreActors = false, bool ignoreTerrain = false);

        /// Get normalized screen coordinates of the bounding box's summit, where (0,0) is the top left corner
        osg::Vec2f getScreenCoords(const osg::BoundingBox& bb);

        void setSkyEnabled(bool enabled);

        bool toggleRenderMode(RenderMode mode);

        void spawnEffect(VFS::Path::NormalizedView model, std::string_view texture, const osg::Vec3f& worldPosition,
            float scale = 1.f, bool isMagicVFX = true, bool useAmbientLight = true, std::string_view effectId = {},
            bool loop = false);

        void removeEffect(std::string_view effectId);

        /// Clear all savegame-specific data
        void clear();

        /// Clear all worldspace-specific data
        void notifyWorldSpaceChanged();

        /// The player was put somewhere rather than walked there, inside a worldspace the
        /// renderer is still drawing: `ActionTeleport` — a door, `coc`, Recall, a boat. The
        /// world's effects stay, which is what tells it from `notifyWorldSpaceChanged`; the
        /// renderer is told the same thing either way.
        void notifyTeleport();

        void update(float dt, bool paused);

        /// Describes this frame and hands it to the renderer, then asks for it drawn: two calls,
        /// because the description has to land before the scene's update traversal and the drawing
        /// after it. See `Renderer::describeFrame` and `Renderer::renderFrame`.
        void describeFrame();
        void renderFrame();

        Animation* getAnimation(const MWWorld::Ptr& ptr);
        const Animation* getAnimation(const MWWorld::ConstPtr& ptr) const;

        PostProcessor* getPostProcessor();

        void addWaterRippleEmitter(const MWWorld::Ptr& ptr);
        void removeWaterRippleEmitter(const MWWorld::Ptr& ptr);
        void emitWaterRipple(const osg::Vec3f& pos);

        void updatePlayerPtr(const MWWorld::Ptr& ptr);

        void removePlayer(const MWWorld::Ptr& player);
        void setupPlayer(const MWWorld::Ptr& player);
        void renderPlayer(const MWWorld::Ptr& player);

        void rebuildPtr(const MWWorld::Ptr& ptr);

        void processChangedSettings(const Settings::CategorySettingVector& settings);

        float getNearClipDistance() const { return mNearClip; }
        float getViewDistance() const { return mViewDistance; }

        void setViewDistance(float distance, bool delay = false);

        float getTerrainHeightAt(const osg::Vec3f& pos, ESM::RefId worldspace);

        // camera stuff
        Camera* getCamera() { return mCamera.get(); }

        /// temporarily override the field of view with given value.
        void overrideFieldOfView(float val);
        void setFieldOfView(float val);
        float getFieldOfView() const;
        /// reset a previous overrideFieldOfView() call, i.e. revert to field of view specified in the settings file.
        void resetFieldOfView();

        osg::Vec3f getHalfExtents(const MWWorld::ConstPtr& object) const;

        // Return local bounding box. Safe to be called in parallel with cull thread.
        osg::BoundingBox getCullSafeBoundingBox(const MWWorld::Ptr& ptr) const;

        void exportSceneGraph(
            const MWWorld::Ptr& ptr, const std::filesystem::path& filename, const std::string& format);

        LandManager* getLandManager() const;

        bool toggleBorders();

        void updateActorPath(const MWWorld::ConstPtr& actor, const std::deque<osg::Vec3f>& path,
            const DetourNavigator::AgentBounds& agentBounds, const osg::Vec3f& start, const osg::Vec3f& end) const;

        void removeActorPath(const MWWorld::ConstPtr& actor) const;

        void setNavMeshNumber(const std::size_t value);

        void setActiveGrid(const osg::Vec4i& grid);

        bool pagingEnableObject(int type, const MWWorld::ConstPtr& ptr, bool enabled);
        void pagingBlacklistObject(int type, const MWWorld::ConstPtr& ptr);
        bool pagingUnlockCache();
        void getPagedRefnums(const osg::Vec4i& activeGrid, std::vector<ESM::RefNum>& out);

        void updateProjectionMatrix();

        void setNavMeshMode(Settings::NavMeshRenderMode value);

        void setProjectionOffset(const osg::Vec2f& offset)
        {
            mProjectionOffset = offset;
            mUpdateProjectionMatrix = true;
        }
        osg::Vec2f getProjectionOffset() const { return mProjectionOffset; }

    private:
        /// See EyeState in sceneframe.hpp
        EyeState describeEye() const;

        void updateTextureFiltering();
        void updateAmbient();

        Ground& getGround(ESM::RefId worldspace);

        void reportStats() const;

        void updateNavMesh();

        void updateRecastMesh();

        osg::ref_ptr<osgUtil::IntersectionVisitor> getIntersectionVisitor(osgUtil::Intersector* intersector,
            bool ignorePlayer, bool ignoreActors, bool ignoreTerrain, std::span<const MWWorld::Ptr> ignoreList = {});

        osg::ref_ptr<IntersectionVisitorWithIgnoreList> mIntersectionVisitor;

        Renderer& mRenderer;
        osg::ref_ptr<osg::Group> mRootNode;
        osg::ref_ptr<osg::Group> mSceneRoot;
        Resource::ResourceSystem* mResourceSystem;

        osg::ref_ptr<SceneUtil::WorkQueue> mWorkQueue;

        osg::ref_ptr<SceneUtil::Light> mSunLight;

        DetourNavigator::Navigator& mNavigator;
        std::unique_ptr<NavMesh> mNavMesh;
        std::size_t mNavMeshNumber = 0;
        std::unique_ptr<ActorsPaths> mActorsPaths;
        std::unique_ptr<RecastMesh> mRecastMesh;
        std::unique_ptr<Pathgrid> mPathgrid;
        std::unique_ptr<Objects> mObjects;
        std::unordered_map<ESM::RefId, std::unique_ptr<Ground>> mGrounds;
        /// The current worldspace's ground, and the terrain it holds: what the game drives and
        /// tells about the distance.
        Ground* mGround;
        Terrain::World* mTerrain;
        std::unique_ptr<TerrainStorage> mTerrainStorage;
        ObjectStorage mObjectStorage;
        std::unique_ptr<Precipitation> mPrecipitation;
        std::unique_ptr<FogManager> mFog;
        std::unique_ptr<EffectManager> mEffectManager;
        osg::ref_ptr<NpcAnimation> mPlayerAnimation;
        osg::ref_ptr<SceneUtil::PositionAttitudeTransform> mPlayerNode;
        std::unique_ptr<Camera> mCamera;

        /// What the game decides about the frame beyond what the objects above hold — the water,
        /// the sun's visibility, the sky's switch, the moon's paint, the projection and the step —
        /// and the frame itself, described from them and handed to the renderer.
        FrameDescriber mFrame;

        osg::Vec4f mAmbientColor;
        float mNightEyeFactor;

        float mNearClip;
        float mViewDistance;
        bool mFieldOfViewOverridden;
        float mFieldOfViewOverride;
        float mFieldOfView;
        float mFirstPersonFieldOfView;
        bool mUpdateProjectionMatrix = false;
        osg::Vec2f mProjectionOffset;
        const MWWorld::GroundcoverStore& mGroundCoverStore;

        void operator=(const RenderingManager&);
        RenderingManager(const RenderingManager&);
    };

}

#endif
