#ifndef GAME_RENDER_RENDERER_H
#define GAME_RENDER_RENDERER_H

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <string_view>
#include <vector>

#include <osg/Timer>
#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <components/esm3/refnum.hpp>
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

namespace osgGA
{
    class EventQueue;
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

namespace Resource
{
    class ResourceSystem;
}

namespace SceneUtil
{
    class AsyncScreenCaptureOperation;
}

namespace Shader
{
    class ShaderManager;
}

namespace VFS
{
    class Manager;
}

namespace MWWorld
{
    class CellStore;
    class Ptr;
}

namespace MWRender
{
    class OffscreenView;
    struct OffscreenViewSpec;
    class SubjectView;
    class PostProcessor;
    class RenderingManager;
    struct SceneFrame;
    struct RtxSetup;

    /// What every renderer needs to exist, whatever it draws with.
    struct RendererSpec
    {
        /// Where a renderer's own files are read from: the ray tracer's shaders.
        std::filesystem::path mResourceDir;

        /// Where a renderer keeps what it compiled: regenerable, so the cache directory.
        std::filesystem::path mCachePath;

        /// What a harness run asks of the ray tracer, or null. `GlRenderer` ignores it.
        const RtxSetup* mRtx = nullptr;
    };

    /// One image of the world on the screen, and the window it goes in. Nothing below this line is
    /// abstracted — contexts, swapchains, render bins and acceleration structures belong to a
    /// renderer outright, and an interface over them would be a mini-GL that Vulkan does not fit.
    /// A pure virtual is a question both renderers answer. A default is the answer of the renderer
    /// the question is not about — nothing, for a compile operation the ray tracer has not got, or
    /// for a schedule the rasterizer does not run — and the other one overrides it.
    class Renderer
    {
    public:
        virtual ~Renderer();

        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        /// The resource system exists; keep it, and tell its scene manager what this renderer needs
        /// of every model it loads. The rasterizer sets how many textures a shader may sample and
        /// the switches its shader visitor reads; the ray tracer turns that visitor off, because it
        /// compiles no GLSL and reads a model's state as the loader left it. Called once, before any
        /// model is loaded and before anything below asks for a picture, and the one place a
        /// renderer is handed the resource system: it outlives the renderer and never changes.
        virtual void prepareResources(Resource::ResourceSystem& resources) = 0;

        /// The window the renderer made, for input, the GUI's scale and the gamma ramp.
        virtual SDL_Window* getWindow() const = 0;

        /// The ground of one worldspace, as this renderer draws it. The rasterizer builds
        /// upstream's chunked world with its paging and groundcover; a renderer that stands the
        /// ground itself hands back a `Terrain::World` that holds the storage, the worldspace and
        /// the active grid and builds nothing, because chunks beside `Rtx::CellRing` would be built
        /// for nobody, inside `Scene::changeCellGrid`'s synchronous wait. Once per worldspace.
        virtual Ground createGround(const GroundSpec& spec) = 0;

        /// A script has disabled one reference, or enabled it again — for a renderer standing the
        /// distance itself. The paging is told by its own route.
        virtual void enableReference(ESM::RefNum refnum, bool enabled) {}

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

        /// The camera, the frame stamp, the input queue and the stats, adopted from whichever
        /// renderer made them and read by the game whatever draws. The queue is null under a
        /// renderer with no viewer, and its readers treat null as nothing to drain: a queue kept
        /// for them would allocate an adapter per frame that nobody reads.
        osg::Camera& getCamera() const;
        osg::FrameStamp& getFrameStamp() const;
        osgGA::EventQueue* getEvents() const { return mEvents.get(); }
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
        virtual void showWorld(bool shown) = 0;

        /// A render mode this renderer owns, turned off and back on, and which it now is:
        /// `Render_Scene` is the `tws` console command — a second reason apart from `showWorld`,
        /// because a loading screen that ends while `tws` is off must not bring the world back —
        /// and `Render_Wireframe` is the rasterizer's polygon mode. The game keeps the modes that
        /// are its own nodes (paths, meshes, the pathgrid) and the water.
        virtual bool toggleRenderMode(RenderMode mode) = 0;

        /// The shader chain over the frame, or null where this renderer has none. Owned here,
        /// because what happens between the scene and the screen is the whole of what a renderer is
        /// for; every reader already treats null as "no chain".
        virtual PostProcessor* getPostProcessor() { return nullptr; }

        /// The one point in the frame where the world is the calling thread's alone, for a renderer
        /// that has a schedule to run against it: the harness's `RtxTool::Session` teleports, aims a
        /// camera and turns a sky, and each is a change to the simulation. Called from
        /// `Engine::frame` and from nowhere else, because a loading screen drives `advance` and
        /// `updateTraversal` for frames of its own and a teleport made from inside one re-enters it.
        virtual void tickSchedule() {}

        /// Opens the frame's clock and says how long the frame stands for, in seconds: what the wall
        /// measured, unless a renderer that has to repeat itself keeps a clock of its own
        /// (`Rtx::FrameClock`).
        virtual double beginFrame(double measured) { return measured; }

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

        /// The world under the camera has been replaced rather than moved through, which a ray
        /// tracer reconstructing across frames needs telling and a rasterizer does not.
        virtual void notifyWorldSpaceChanged() {}

        /// A picture made somewhere other than the eye, for the GUI to show. What goes in the
        /// picture arrives in the spec; how it is drawn is the renderer's, which hands back a
        /// `MyGUI::ITexture` so nothing above this line knows which. Two calls for the two kinds:
        /// a tile of the world, whose spec names the world's own scene, and a subject the game
        /// assembled for the picture — the inventory doll, the race preview — which is also
        /// resized, rebuilt and picked at.
        virtual std::unique_ptr<OffscreenView> createWorldView(const OffscreenViewSpec& spec) = 0;
        virtual std::unique_ptr<SubjectView> createSubjectView(const OffscreenViewSpec& spec) = 0;

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

        /// Whether the window has been closed. A renderer whose window is closed by SDL's own quit
        /// event answers no.
        virtual bool done() const { return false; }

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

        /// The operation an OSG loader compiles through, or null: what `Resource::SceneManager`, the
        /// paging and the loading screen's per-frame budget go through.
        virtual osgUtil::IncrementalCompileOperation* getCompileOperation() const { return nullptr; }

        virtual void setVSync(SDLUtil::VSyncMode mode) = 0;

        /// Settings the player changed in the menu, as `Settings::Manager` reports them. Each
        /// renderer picks out its own — the rasterizer its shader chain, the ray tracer its
        /// upscaler — and the game never learns which setting belongs to whom.
        virtual void processChangedSettings(const Settings::CategorySettingVector& changed) {}

        /// Recompiles whatever GLSL has been edited since the last call, once per frame. A renderer
        /// drawing with compiled SPIR-V has nothing to reload.
        virtual void reloadChangedShaders(Shader::ShaderManager& shaders) {}

        /// The origin the per-frame profiler measures from, so its spans land on the same axis as
        /// the renderer's own counters.
        virtual osg::Timer_t getStartTick() const = 0;

        /// The overlay the debug keys toggle and the per-frame dump `OPENMW_OSG_STATS_FILE` asks for:
        /// the OSG stats overlay is the rasterizer's, and a renderer with its own frame times has
        /// nothing to install.
        virtual void installStatsOverlay(const VFS::Manager& vfs, bool toFile) {}
        virtual void reportStats(unsigned frameNumber, std::ostream& stream) const {}

        /// MyGUI's backend, a second implementation of MyGUI's own interface. Called at the main
        /// menu, before there is a world, off the resource system `prepareResources` kept.
        virtual std::unique_ptr<MyGUIPlatform::Platform> createGuiPlatform(osg::Group& guiRoot, float scalingFactor,
            VFS::Path::NormalizedView resourcePath, const std::filesystem::path& logPath)
            = 0;

    protected:
        Renderer() = default;

        /// Taken from whatever made them, once, before anything asks. The ray tracer has no event
        /// queue, because what SDL would put in one is read by `osgViewer` handlers it has not got.
        void adopt(osg::Camera& camera, osg::FrameStamp& frameStamp, osgGA::EventQueue* events, osg::Stats& stats);

        /// Parents the root where this renderer's traversals start from: the viewer's scene data,
        /// or the camera the ray tracer walks from.
        virtual void adoptTraversalRoot(osg::Group& root) = 0;

        /// The view mask has changed; put it where this renderer reads it from.
        virtual void applyViewMask(unsigned int mask) = 0;

        SceneUtil::AsyncScreenCaptureOperation& getScreenshotWriter() const;

    private:
        osg::ref_ptr<SceneUtil::AsyncScreenCaptureOperation> mScreenshotWriter;
        osg::ref_ptr<osg::Camera> mCamera;
        osg::ref_ptr<osg::FrameStamp> mFrameStamp;
        osg::ref_ptr<osgGA::EventQueue> mEvents;
        osg::ref_ptr<osg::Stats> mStats;
        osg::ref_ptr<osg::Group> mTraversalRoot;
        unsigned int mViewMask = ~0u;
    };

    /// The one place the choice is made. Throws naming the name where there is no such renderer,
    /// because a fallback would answer "why does it look like that" with silence.
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

    /// What every renderer asks SDL for, out of the video settings, and the hints SDL reads inside
    /// `SDL_CreateWindow` and so has to be given first. `surfaceFlag` names what is drawn into the
    /// surface: `SDL_WINDOW_OPENGL` for the rasterizer.
    WindowPlacement describeWindow(std::uint32_t surfaceFlag);

}

#endif
