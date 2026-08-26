#ifndef OPENMW_MWRENDER_RENDERINGMANAGER_H
#define OPENMW_MWRENDER_RENDERINGMANAGER_H

#include "objects.hpp"
#include "objectstorage.hpp"
#include "renderinginterface.hpp"
#include "rendermode.hpp"
#include "sceneframe.hpp"

#include "weatherresult.hpp"
#include <components/rtx/terrainresidency.hpp>

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

namespace ESM
{
    struct Cell;
    struct FormId;
    using RefNum = FormId;
}

namespace Fx
{
    class StateUpdater;
}

namespace Terrain
{
    class ObjectPaging;
    class World;
}

namespace Fallback
{
    class Map;
}

namespace SceneUtil
{
    class ShadowManager;
    class WorkQueue;
    class LightManager;
    class UnrefQueue;
    class PerViewUniformStateUpdater;
    class SharedUniformStateUpdater;
    class StateUpdater;
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

namespace Debug
{
    struct DebugDrawer;
}

namespace Weather
{
    class Precipitation;
}

namespace MWRender
{

    class IntersectionVisitorWithIgnoreList;

    class EffectManager;
    class FogManager;
    class SkyManager;
    class NpcAnimation;
    class Pathgrid;
    class Camera;
    class Water;
    class TerrainStorage;
    class LandManager;
    class NavMesh;
    class ActorsPaths;
    class RecastMesh;
    class Groundcover;
    class PostProcessor;
    class Renderer;
    class Stage;

    class RenderingManager : public MWRender::RenderingInterface
    {
    public:
        RenderingManager(Renderer& renderer, Stage& stage, osg::ref_ptr<osg::Group> rootNode,
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

        SceneUtil::LightManager* getLightRoot();

        void setNightEyeFactor(float factor);

        void setAmbientColour(const osg::Vec4f& colour);

        int skyGetMasserPhase() const;
        int skyGetSecundaPhase() const;
        void skySetMoonColour(bool red);

        const osg::Vec4f& getSunLightPosition() const;
        void setSunDirection(const osg::Vec3f& direction);
        void setSunColour(const osg::Vec4f& diffuse, const osg::Vec4f& specular, float sunVis);
        void setNight(bool isNight) { mNight = isNight; }

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

        /// What the weather drops, for whoever is drawing it. Null before the sky is built.
        ///
        /// **Owned by the sky manager and drawn by both**, which is the whole point of it being an
        /// `osgParticle` system and not a renderer's own: there is one rain and one storm cloud, and
        /// the ray tracer walks the same nodes the rasterizer does rather than making a second set.
        Weather::Precipitation* getPrecipitation();

        /// What the weather system has just worked out, for whatever draws the sky.
        ///
        /// **Forwarded rather than reached through.** `MWWorld::WeatherManager` already tells this
        /// object about the fog, the ambient and the sun; these are the rest of the same sentence,
        /// and routing them here is what keeps `SkyManager` — which is one renderer's — from being
        /// named outside `mwrender`.
        void setWeather(const WeatherResult& weather);
        void setStormParticleDirection(const osg::Vec3f& direction);
        void setSunVisible(bool visible);
        void setGlareTimeOfDayFade(float fade);
        void setMoonStates(const MoonState& masser, const MoonState& secunda);

        bool toggleRenderMode(RenderMode mode);

        SkyManager* getSkyManager();

        void spawnEffect(VFS::Path::NormalizedView model, std::string_view texture, const osg::Vec3f& worldPosition,
            float scale = 1.f, bool isMagicVFX = true, bool useAmbientLight = true, std::string_view effectId = {},
            bool loop = false);

        void removeEffect(std::string_view effectId);

        /// Clear all savegame-specific data
        void clear();

        /// Clear all worldspace-specific data
        void notifyWorldSpaceChanged();

        void update(float dt, bool paused);

        /// Describes this frame — the world, the eye, the clock and the light on it — and asks the
        /// renderer for it. Every frame the main loop runs; the four places that draw a GUI over no
        /// world call `Renderer::renderGui` instead.
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

        /// How far from the eye the world's ground is built, in units, or nought where it reaches no
        /// further than the cells the simulation has loaded.
        ///
        /// **What the local map is a map of.** Upstream sizes it from `distant terrain` and
        /// `viewing distance`, which are the rasterizer's two answers to this one question. A ray
        /// tracer pages whatever the first says and measures itself against `distant land cells`
        /// rather than the second, so a map built from the settings shows less ground than there is.
        float getTerrainReach() const;

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

        Debug::DebugDrawer& getDebugDrawer() const { return *mDebugDraw; }

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

        void setScreenRes(int width, int height);

        void setNavMeshMode(Settings::NavMeshRenderMode value);

        void setProjectionOffset(const osg::Vec2f& offset)
        {
            mProjectionOffset = offset;
            mUpdateProjectionMatrix = true;
        }
        osg::Vec2f getProjectionOffset() const { return mProjectionOffset; }

    private:
        /// What the world is doing this frame, gathered from where each part of it settled.
        ///
        /// **Once, and in the world's own numbers.** Two renderers wanted the same twenty facts in
        /// two different spellings, and answering them separately meant asking the sun, the fog and
        /// the sky twice a frame through two channels pointing opposite ways. There is one channel
        /// and it points down.
        WorldState describeWorld() const;

        void updateTextureFiltering();
        void updateAmbient();
        struct WorldspaceChunkMgr
        {
            std::unique_ptr<Terrain::World> mTerrain;
            std::unique_ptr<Terrain::ObjectPaging> mObjectPaging;
            std::unique_ptr<Groundcover> mGroundcover;
        };

        WorldspaceChunkMgr& getWorldspaceChunkMgr(ESM::RefId worldspace);

        void reportStats() const;

        void updateNavMesh();

        void updateRecastMesh();

        const bool mSkyBlending;

        osg::ref_ptr<osgUtil::IntersectionVisitor> getIntersectionVisitor(osgUtil::Intersector* intersector,
            bool ignorePlayer, bool ignoreActors, bool ignoreTerrain, std::span<const MWWorld::Ptr> ignoreList = {});

        osg::ref_ptr<IntersectionVisitorWithIgnoreList> mIntersectionVisitor;

        Renderer& mRenderer;
        Stage& mStage;
        osg::ref_ptr<osg::Group> mRootNode;
        osg::ref_ptr<SceneUtil::LightManager> mSceneRoot;
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
        std::unique_ptr<Water> mWater;
        std::unordered_map<ESM::RefId, WorldspaceChunkMgr> mWorldspaceChunks;
        Terrain::World* mTerrain;

        /// Where the terrain's own chunks are, for the renderer that walks rather than culls. Costs
        /// a `Terrain::View` and nothing at all where the terrain parents its chunks.
        Rtx::TerrainResidency mResident;

        std::unique_ptr<TerrainStorage> mTerrainStorage;

        // What the paging reads the world out of. Held here because every worldspace's paging
        // borrows it, and it outlives all of them.
        ObjectStorage mObjectStorage;
        Terrain::ObjectPaging* mObjectPaging;
        Groundcover* mGroundcover;
        std::unique_ptr<SkyManager> mSky;
        std::unique_ptr<FogManager> mFog;

        std::unique_ptr<EffectManager> mEffectManager;
        std::unique_ptr<SceneUtil::ShadowManager> mShadowManager;

        /// Where the sun is, as the world last decided.
        ///
        /// **Held rather than pushed.** Two paths set it — an exterior's orbit and an interior's
        /// fixed nonsense angle — and the direction the light travels is not the direction the sun
        /// is drawn at whenever `match sunlight to sun` is off, so `mSunLight` is not a record of
        /// it. Nothing but this remembers.
        osg::Vec4f mSunPosition;
        osg::Vec4f mSunVector;
        bool mSunAtNight = false;

        float mSunVisibility = 0.f;

        /// What the disc is painted with, and how much of the sun is over the horizon in `w`.
        ///
        /// **The other sun colour, and the one that is about the sun.** `mSunLight`'s diffuse is
        /// what the world receives — sun and sky together, blue at night because that is the sky —
        /// while this is white through the day and warms only as it goes down. `Sky::sunDiscAt`
        /// builds the colour and `Sky::sunShareAt` the share; this is the copy `WorldState` reports,
        /// so a renderer that draws its own disc is not left inferring one from the light.
        osg::Vec4f mSunDiscColour{ 1.0f, 1.0f, 1.0f, 0.0f };

        /// How much of the sun the weather lets through, which dims a disc under an overcast.
        float mSunGlare = 1.f;

        /// The deck's own crossing between two weathers' cloud textures, which is not the plain
        /// transition factor, and how far the stars have come out. Both `WorldState`'s to report.
        float mCloudBlend = 0.f;
        float mNightFade = 0.f;

        /// What the sky and the cloud deck are coloured by, and how fast the deck runs.
        ///
        /// **Off the weather rather than out of `SkyManager`.** That manager is one renderer's and
        /// is built lazily; a ray-traced frame reading its cached copies got whatever an unbuilt one
        /// starts at, which for a colour is black.
        osg::Vec4f mSkyColour;
        osg::Vec4f mCloudFog;
        float mCloudSpeed = 0.f;

        /// How far the deck has scrolled and the stars have rolled. **One owner and two consumers**:
        /// the sky manager is handed it and `WorldState` reports it, so both renderers turn one sky.
        Sky::SkyRoll mSkyRoll;

        bool mWaterEnabled = false;
        float mWaterHeight = 0.f;
        osg::ref_ptr<NpcAnimation> mPlayerAnimation;
        osg::ref_ptr<SceneUtil::PositionAttitudeTransform> mPlayerNode;
        std::unique_ptr<Camera> mCamera;
        osg::ref_ptr<Debug::DebugDrawer> mDebugDraw;

        osg::ref_ptr<SceneUtil::StateUpdater> mStateUpdater;
        osg::ref_ptr<SceneUtil::SharedUniformStateUpdater> mSharedUniformStateUpdater;
        osg::ref_ptr<SceneUtil::PerViewUniformStateUpdater> mPerViewUniformStateUpdater;

        osg::Vec4f mAmbientColor;
        float mNightEyeFactor;

        float mNearClip;
        float mViewDistance;
        bool mFieldOfViewOverridden;
        float mFieldOfViewOverride;
        float mFieldOfView;
        float mFirstPersonFieldOfView;
        bool mUpdateProjectionMatrix = false;
        bool mNight = false;

        /// Masser and Secunda, as the weather system last settled them.
        ///
        /// **Kept for the reason the storm's direction is:** `WorldState` has to report them and the
        /// sky cannot be asked back. Until the weather system first speaks they are an alpha of
        /// nothing, which is a moon nothing draws.
        MoonState mMoonStates[2] = {};

        /// The last direction `MWWorld::WeatherManager` aimed its storm particles.
        ///
        /// **Kept because `WorldState` has to report it and the sky cannot be asked.** The weather
        /// system computes it once a frame against the player's position and hands it straight on;
        /// the value is `Weather::defaultStormDirection` until it first does.
        osg::Vec3f mStormParticleDirection = osg::Vec3f(0.0f, 1.0f, 0.0f);

        osg::Vec2f mProjectionOffset;
        const MWWorld::GroundcoverStore& mGroundCoverStore;
        std::map<std::string, std::string> mAppliedShadowDefines;

        void operator=(const RenderingManager&);
        RenderingManager(const RenderingManager&);
    };

}

#endif
