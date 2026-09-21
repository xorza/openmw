#ifndef GAME_RENDER_RENDERER_H
#define GAME_RENDER_RENDERER_H

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

#include <osg/Timer>
#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <components/misc/frameratelimiter.hpp>
#include <components/sdlutil/graphicslistener.hpp>
#include <components/sdlutil/vsyncmode.hpp>
#include <components/settings/categories.hpp>
#include <components/vfs/pathutil.hpp>

#include "ground.hpp"
#include "rendermode.hpp"

struct SDL_Window;

namespace osg
{
    class Camera;
    class FrameStamp;
    class Group;
    class Image;
    class Stats;
}

namespace osgUtil
{
    class IncrementalCompileOperation;
}

namespace MyGUI
{
    class ITexture;
}

namespace MyGUIPlatform
{
    class Platform;
}

namespace Misc
{
    class FrameClock;
}

namespace Resource
{
    class ResourceSystem;
}

namespace SceneUtil
{
    class AsyncScreenCaptureOperation;
}

namespace MWWorld
{
    class CellStore;
    class Ptr;
}

namespace MWRender
{
    class MapOverlay;
    struct MapOverlaySpec;
    class OffscreenView;
    struct OffscreenViewSpec;
    class SubjectView;
    class PostProcessor;
    class RenderingManager;
    struct SceneFrame;

    /// What a renderer is given to exist. The rasterizer reads none of it: its shaders come
    /// through the resource system's shader path and it compiles nothing it keeps.
    struct RendererSpec
    {
        /// Where a renderer's own files are read from: the ray tracer's shaders.
        std::filesystem::path mResourceDir;

        /// Where a renderer keeps what it compiled: regenerable, so the cache directory.
        std::filesystem::path mCachePath;
    };

    /// One image of the world on the screen, and the window it goes in. Nothing below this line is
    /// abstracted — contexts, swapchains, render bins and acceleration structures belong to a
    /// renderer outright, and an interface over them would be a mini-GL that Vulkan does not fit.
    /// Every member is a question the game asks; a pure virtual is one both renderers answer, and
    /// a default is an empty answer — a loading budget the ray tracer has no compiler to spend, or
    /// a world-space change the rasterizer has no history to lose. What the game must never be handed is one
    /// renderer's mechanism to poke at, because every caller of that grows a null test that is a
    /// renderer test in disguise; the two that remain, the shader chain and the compile operation,
    /// are there for upstream callers that cannot be changed.
    ///
    /// The window's own moments — its size, the function keys — reach the renderer as an
    /// `SDLUtil::GraphicsListener`, from `SDLUtil::InputWrapper`.
    class Renderer : public SDLUtil::GraphicsListener
    {
    public:
        virtual ~Renderer();

        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        /// The resource system exists; keep it, and tell its scene manager what this renderer needs
        /// of every model it loads. Called once, before any model is loaded and before anything
        /// below asks for a picture, and the one place a renderer is handed the resource system: it
        /// outlives the renderer and never changes, so the base keeps it for both.
        void prepareResources(Resource::ResourceSystem& resources);

        /// The window the renderer made, for input, the GUI's scale and the gamma ramp.
        virtual SDL_Window* getWindow() const = 0;

        /// The ground of one worldspace and the distance over it, as this renderer draws them. The
        /// rasterizer builds upstream's chunked world with its paging and groundcover; a renderer
        /// that stands the ground itself hands back a `Terrain::World` that holds the storage, the
        /// worldspace and the active grid and builds nothing, because chunks beside `Rtx::CellRing`
        /// would be built for nobody, inside `Scene::changeCellGrid`'s synchronous wait. Once per
        /// worldspace, and everything the game says about the distance from then on — a script's
        /// toggle, a moved object, a new game — is said to what this hands back.
        virtual std::unique_ptr<Ground> createGround(const GroundSpec& spec) = 0;

        /// The world's own events, as `RenderingManager` receives them: a cell comes and goes, an
        /// actor that makes ripples comes and goes, something splashes. The rasterizer's water
        /// listens; a renderer with no effect to hang on them hears nothing.
        virtual void addCell(const MWWorld::CellStore* cell) {}
        virtual void removeCell(const MWWorld::CellStore* cell) {}
        virtual void addWaterRippleEmitter(const MWWorld::Ptr& ptr) {}
        virtual void removeWaterRippleEmitter(const MWWorld::Ptr& ptr) {}
        virtual void emitWaterRipple(const osg::Vec3f& position) {}

        /// What this renderer would like loaded before the first cell is: the rasterizer's sky and
        /// water meshes and textures.
        virtual void listAssetsToPreload(
            std::vector<VFS::Path::Normalized>& models, std::vector<VFS::Path::Normalized>& textures)
        {
        }

        /// The world is going, so anything of it a renderer reads from a thread of its own is let go
        /// of first.
        virtual void detachWorld() {}

        /// How far from the eye ground is built, straight ahead: what the local map is a map of.
        /// Nought where the ground reaches no further than the cells the simulation has loaded.
        virtual float getGroundReach() const = 0;

        /// The node the game hangs the world under: the rasterizer's light manager, with the
        /// lighting method it draws with, or a plain group for a renderer that gathers lights on its
        /// own walk. The game names it, masks it and builds under it; it never asks which it got.
        virtual osg::ref_ptr<osg::Group> createSceneRoot() = 0;

        /// The world exists; build whatever goes between it and the screen. A second phase because
        /// the renderer is made before there is a world, and what the rasterizer puts in front of
        /// the world needs the world to talk to. Called once, from `RenderingManager`'s constructor.
        /// `world` is for what is built once off it — upstream's `PostProcessor`, whose constructor
        /// takes it, the sun light the light manager is given, the precipitation root the
        /// rasterizer puts state on, the resource system the ray tracer's pictures resolve through.
        /// What changes per frame comes through `describeFrame` and never through this reference.
        virtual void attachWorld(RenderingManager& world, osg::Group& worldRoot) = 0;

        /// Whatever the renderer wants culled and drawn, from the top — the rasterizer's
        /// post-processing group rather than the world's own root. Not the game's "Scene Root",
        /// which is what `createSceneRoot` made and hangs somewhere under this.
        void setTraversalRoot(osg::Group& root);

        /// The camera, the frame stamp and the stats, adopted from whichever renderer made them
        /// and read by the game whatever draws.
        osg::Camera& getCamera() const;
        osg::FrameStamp& getFrameStamp() const;
        osg::Stats& getStats() const;

        osg::Group& getTraversalRoot() const;

        /// What the eye sees, as the bits of `MWRender::VisMask` the game has switched on: the
        /// one vocabulary both renderers read, the rasterizer as its cull mask and the ray tracer
        /// as `rayMaskOf`. Kept here, so a screen that covers the world (`showWorld`) parks the
        /// rasterizer's camera without the game's answer moving.
        void setViewMask(unsigned int mask);
        unsigned int getViewMask() const { return mViewMask; }

        /// Whether the world is being shown at all; the interface is drawn either way. A loading
        /// screen and the main menu's cover say no, and want nothing animating behind them. Said as
        /// intent, because the rasterizer answers it with its cull masks and the ray tracer by not
        /// walking the scene.
        void showWorld(bool shown);
        bool isWorldShown() const { return mWorldShown; }

        /// A render mode this renderer owns, turned off and back on, and which it now is:
        /// `Render_Scene` is the `tws` console command — a second reason apart from `showWorld`,
        /// because a loading screen that ends while `tws` is off must not bring the world back,
        /// and a world `tws` hides is still updated — and the rest are the renderer's own
        /// (`Render_Wireframe` is the rasterizer's polygon mode). The game keeps the modes that
        /// are its own nodes (paths, meshes, the pathgrid) and the water.
        bool toggleRenderMode(RenderMode mode);
        bool isWorldToggled() const { return mWorldToggled; }

        /// Whether a frame draws the world: both of the answers above, said once so a frame cannot
        /// walk on one and trace on the other.
        bool drawsWorld() const { return mWorldShown && mWorldToggled; }

        /// The shader chain over the frame, or null where this renderer has none. Owned here,
        /// because what happens between the scene and the screen is the whole of what a renderer is
        /// for. Its readers are the chain's own window (`PostProcessorHud`), the key that opens it,
        /// the `openmw.postprocessing` package every renderer owes a script, and the rasterizer's
        /// screenshot — each an upstream caller that tests null once. What a script asks of a
        /// renderer itself goes through the three members below and never through this.
        virtual PostProcessor* getPostProcessor() { return nullptr; }

        /// Recompiles every shader this renderer draws with, from source: the debug package's
        /// `triggerShaderReload`. The rasterizer recompiles its GLSL and its chain.
        virtual void reloadShaders() {}

        /// Whether a shader edited on disk is recompiled as it changes, `setShaderHotReloadEnabled`.
        virtual void setLiveShaderReload(bool enabled) {}

        /// The scripts were reset, so nothing a script asked of this renderer holds: the rasterizer
        /// drops the techniques scripts enabled. From `LuaManager::clear`.
        virtual void forgetScriptState() {}

        /// The host's clock: what time it is and how long the frame now open stands for. Handed
        /// over once, before the first frame, and outlives this. The rasterizer's viewer stamps the
        /// wall for itself; the ray tracer stamps what this says, which is what makes a stated step
        /// a run that repeats.
        void setFrameClock(const Misc::FrameClock& clock) { mClock = &clock; }

        /// `[Video] framerate limit`, in frames a second, or nought for none. Once, beside the
        /// clock: the setting is the launcher's and is not offered while the game runs.
        void setFrameRateLimit(float limit);

        /// Holds the game until the next frame may begin, and says how long the last one stood
        /// for on the wall. The frame-rate limit lives here, and so does whatever pacing a
        /// renderer has beyond it: a driver that says when to start the frame answers this. Once
        /// per loop, before input is read, because what is read after this is what the frame
        /// shows. The default is the limiter the engine's loop used to hold, one call earlier in
        /// the loop, which is the same point in the cycle.
        virtual std::chrono::steady_clock::duration awaitFrame();

        /// Stamps the next frame. Simulation time stops when the game is paused; reference time
        /// does not.
        virtual void advance(double simulationTime) = 0;

        virtual void eventTraversal() = 0;
        virtual void updateTraversal() = 0;

        /// What the world settled on this frame, once per frame from the main loop, between the
        /// event and the update traversal: after the first because a window resize lands there and
        /// the eye's numbers follow it, before the second because the rasterizer's uniforms are
        /// update callbacks that read what they were last told. The same frame comes back to
        /// `renderFrame`. The default is the ray tracer's answer, which reads the frame when it draws.
        virtual void describeFrame(const SceneFrame& frame) {}

        /// The world, and the GUI over it. Once per frame, from the main loop, after the update
        /// traversal, with the frame `describeFrame` was handed.
        virtual void renderFrame(const SceneFrame& frame) = 0;

        /// The eye did not travel here: a change of worldspace, a teleport inside one, a time
        /// skip. What the last frame showed is not what this one is a step from, which a renderer
        /// reconstructing across frames needs telling and a rasterizer does not. Only the
        /// simulation knows, because a cell load looks like a step from below the seam.
        virtual void notifyCut() {}

        /// A picture made somewhere other than the eye, for the GUI to show. What goes in the
        /// picture arrives in the spec; how it is drawn is the renderer's, which hands back a
        /// `MyGUI::ITexture` so nothing above this line knows which. Two calls for the two kinds:
        /// a tile of the world, whose spec names the world's own scene, and a subject the game
        /// assembled for the picture — the inventory doll, the race preview — which is also
        /// resized, rebuilt and picked at.
        virtual std::unique_ptr<OffscreenView> createWorldView(const OffscreenViewSpec& spec) = 0;
        virtual std::unique_ptr<SubjectView> createSubjectView(const OffscreenViewSpec& spec) = 0;

        /// The world map's overlay, the picture the explored cells are painted into: how a tile
        /// gets into it is the renderer's — a camera's blit, or a composite in main memory — and
        /// `GlobalMap` asks the same of both. Once, when the map window is made.
        virtual std::unique_ptr<MapOverlay> createMapOverlay(const MapOverlaySpec& spec) = 0;

        /// The frame the player was last looking at, held still for the GUI, and taken again from
        /// the next frame drawn every time this is called. Whatever the renderer already has rather
        /// than a copy read back to main memory on the frame a load begins. The bottom row of the
        /// picture is texel row nought, as `createWorldView` promises and `LoadingScreen`
        /// inverts V for; a renderer whose frame arrives the other way up owes the flip here.
        virtual MyGUI::ITexture& freezeFrame() = 0;

        /// The GUI with no world behind it: the loading screen, a modal message box, a video and
        /// the screenshot all draw a frame from inside another one, and none has a world to
        /// describe.
        virtual void renderGui() = 0;

        /// A whole frame of the GUI alone, from inside another frame: the three traversals and then
        /// the advance, in that order, so that the frame number is right for the frame the caller
        /// is in the middle of — see `Engine::go`, which advances first and draws after.
        void renderGuiFrame();

        /// A loading screen has come up, and draws frames of its own through `renderLoadingFrame`
        /// until `endLoading`. The rasterizer hands its compiler the whole of every such frame and
        /// stops recomputing the scene's bound behind the screen; a renderer that compiles nothing
        /// on the frame has nothing to change.
        virtual void beginLoading() {}
        virtual void endLoading() {}

        /// One frame of the loading screen, at the rate the screen is drawn at.
        void renderLoadingFrame(double targetFrameRate);

        /// The frame without the GUI, into an image. The screenshot console command and the save
        /// thumbnails; blocks until the frame it asked for has been drawn.
        virtual void capture(osg::Image& image, int width, int height) = 0;

        /// The screenshot key, which writes a file rather than handing back an image, through the
        /// writer `Engine` handed over.
        virtual void saveScreenshot() = 0;

        /// The writer both renderers hand a captured frame to: `Engine`'s, alive for as long as the
        /// renderer is. Handed over after construction, where upstream built it.
        virtual void setScreenshotWriter(SceneUtil::AsyncScreenCaptureOperation& writer);

        /// Between these two nothing is reading the scene graph, so it can be mutated. A renderer
        /// that draws on the calling thread has nothing to hold still.
        virtual void suspendDraw() {}
        virtual void resumeDraw() {}

        /// The operation an OSG loader compiles through, or null. Kept on the seam for one upstream
        /// caller, `MWWorld::Scene`, which takes it off the scene manager around a load and hands
        /// it back after; what the loading screen wants of it is asked through `beginLoading`.
        virtual osgUtil::IncrementalCompileOperation* getCompileOperation() const { return nullptr; }

        virtual void setVSync(SDLUtil::VSyncMode mode) = 0;

        /// Settings the player changed in the menu, as `Settings::Manager` reports them. Each
        /// renderer picks out its own — the rasterizer its shader chain, the ray tracer its
        /// upscaler — and the game never learns which setting belongs to whom.
        virtual void processChangedSettings(const Settings::CategorySettingVector& changed) {}

        /// The origin the per-frame profiler measures from, so its spans land on the same axis as
        /// the renderer's own counters.
        virtual osg::Timer_t getStartTick() const = 0;

        /// MyGUI's backend, a second implementation of MyGUI's own interface. Called at the main
        /// menu, before there is a world, off the resource system `prepareResources` kept. Where
        /// the interface goes in the graph, if it goes anywhere, is the renderer's to decide.
        virtual std::unique_ptr<MyGUIPlatform::Platform> createGuiPlatform(
            float scalingFactor, VFS::Path::NormalizedView resourcePath, const std::filesystem::path& logPath) = 0;

    protected:
        /// Out of line with the destructor, so a subclass needs none of what the handles point at.
        Renderer();

        /// Taken from whatever made them, once, before anything asks.
        void adopt(osg::Camera& camera, osg::FrameStamp& frameStamp, osg::Stats& stats);

        /// Parents the root where this renderer's traversals start from: the viewer's scene data,
        /// or the camera the ray tracer walks from.
        virtual void adoptTraversalRoot(osg::Group& root) = 0;

        /// `prepareResources`'s hook, with the resource system already kept. The rasterizer sets
        /// how many textures a shader may sample and the switches its shader visitor reads; the ray
        /// tracer turns that visitor off, because it compiles no GLSL and reads a model's state as
        /// the loader left it.
        virtual void configureResources(Resource::ResourceSystem& resources) = 0;

        /// What `prepareResources` kept, for a subclass that resolves a picture, a GUI or a preload
        /// through it. Asserts that it has.
        Resource::ResourceSystem& getResources() const;

        /// What `setFrameClock` handed over. Asserts that it has.
        const Misc::FrameClock& getFrameClock() const;

        /// The view mask has changed; put `getViewMask()` where this renderer reads it from.
        virtual void applyViewMask() = 0;

        /// `isWorldShown` or `isWorldToggled` has changed; put both where this renderer reads
        /// them from.
        virtual void applyWorldShown() = 0;

        /// A render mode other than `Render_Scene`, which the seam answers itself.
        virtual bool toggleOwnRenderMode(RenderMode mode) { return false; }

        /// What `renderLoadingFrame` says before it draws: how long the frame stands for, which is
        /// what the rasterizer's compiler is given to spend on what a loader handed over.
        virtual void applyLoadingBudget(double targetFrameRate) {}

        SceneUtil::AsyncScreenCaptureOperation& getScreenshotWriter() const;

    private:
        Resource::ResourceSystem* mResources = nullptr;
        const Misc::FrameClock* mClock = nullptr;

        /// What the default `awaitFrame` sleeps in and measures by, made anew by `setFrameRateLimit`.
        Misc::FrameRateLimiter mLimiter{ std::chrono::steady_clock::duration::zero() };
        osg::ref_ptr<SceneUtil::AsyncScreenCaptureOperation> mScreenshotWriter;
        osg::ref_ptr<osg::Camera> mCamera;
        osg::ref_ptr<osg::FrameStamp> mFrameStamp;
        osg::ref_ptr<osg::Stats> mStats;
        osg::ref_ptr<osg::Group> mTraversalRoot;
        unsigned int mViewMask = ~0u;

        /// False behind a loading screen and the main menu's cover, where nothing updates.
        bool mWorldShown = true;

        /// False while `tws` is off, where the world updates and is not drawn.
        bool mWorldToggled = true;
    };

    /// The game's own choice, by name. Throws naming the name where there is no such renderer,
    /// because a fallback would answer "why does it look like that" with silence. A host with a
    /// renderer of its own — the harness, with its run — makes it itself, as the engine's host
    /// (`OMW::EngineHost::createRenderer`).
    std::unique_ptr<Renderer> createRenderer(std::string_view name, const RendererSpec& spec);

    /// Where a window goes and what it is, as the video settings ask for it.
    struct WindowPlacement
    {
        int mX = 0;
        int mY = 0;
        int mWidth = 0;
        int mHeight = 0;
        std::uint32_t mFlags = 0;
    };

    /// What every renderer asks SDL for, out of the video settings. `surfaceFlag` names what is
    /// drawn into the surface: `SDL_WINDOW_OPENGL` for the rasterizer.
    WindowPlacement describeWindow(std::uint32_t surfaceFlag);

    /// The hints SDL reads inside `SDL_CreateWindow`, out of the same settings, so a renderer
    /// gives them before it creates its window.
    void applyWindowHints();

}

#endif
