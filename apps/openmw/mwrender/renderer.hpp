#ifndef GAME_RENDER_RENDERER_H
#define GAME_RENDER_RENDERER_H

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <string_view>

#include <osg/Timer>
#include <osg/ref_ptr>

#include <components/esm3/refnum.hpp>
#include <components/sdlutil/vsyncmode.hpp>
#include <components/vfs/pathutil.hpp>

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
    class ImageManager;
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

namespace MWRender
{
    class OffscreenView;
    struct OffscreenViewSpec;
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
    /// A pure virtual is a question both renderers answer; a question only the rasterizer can
    /// answer has a default here, and the ray tracer does not override it.
    class Renderer
    {
    public:
        virtual ~Renderer();

        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        /// How many textures one shader may sample, which `Shader::ShaderManager` reserves its
        /// global units out of. A GL fact, so the default is what a renderer with no GL context
        /// says.
        virtual int getMaxTextureUnits() const { return sAssumedTextureUnits; }

        /// What the shader visitor is told where there is no GL context to ask. It decides only how
        /// many slots the visitor is willing to use; the roles it labels them with are the same for
        /// any number this large.
        static constexpr int sAssumedTextureUnits = 32;

        /// The window the renderer made, for input, the GUI's scale and the gamma ramp.
        virtual SDL_Window* getWindow() const = 0;

        /// Whether the game builds the ground's chunks for this renderer. A renderer that stands the
        /// ground itself answers no and is given a `Terrain::World` that holds the storage, the
        /// worldspace and the active grid and builds nothing: chunks beside `Rtx::CellRing` would be
        /// built for nobody, inside `Scene::changeCellGrid`'s synchronous wait.
        virtual bool buildsTerrainChunks() const { return true; }

        /// A script has disabled one reference, or enabled it again — for a renderer standing the
        /// distance itself. The paging is told by its own route.
        virtual void enableReference(ESM::RefNum refnum, bool enabled) {}

        /// The world is going, so anything of it a renderer reads from a thread of its own is let go
        /// of first.
        virtual void detachWorld() {}

        /// How far from the eye ground is built, straight ahead: what the local map is a map of.
        /// Nought where the ground reaches no further than the cells the simulation has loaded.
        virtual float getGroundReach() const = 0;

        /// The world exists; build whatever goes between it and the screen. A second phase because
        /// the renderer is made before there is a world, and what the rasterizer puts in front of
        /// the world needs the world to talk to. Called once, from `RenderingManager`'s constructor.
        virtual void attachWorld(RenderingManager& world, osg::Group& worldRoot) = 0;

        /// Whatever the renderer wants culled and drawn — the rasterizer's post-processing group
        /// rather than the world's own root.
        void setSceneRoot(osg::Group& root);

        /// The camera, the frame stamp, the input queue and the stats, adopted from whichever
        /// renderer made them and read by the game whatever draws.
        osg::Camera& getCamera() const;
        osg::FrameStamp& getFrameStamp() const;
        osgGA::EventQueue* getEvents() const { return mEvents.get(); }
        osg::Stats& getStats() const;

        osg::Group& getSceneRoot() const;

        /// Whether the world is being shown at all; the interface is drawn either way. A loading
        /// screen and the main menu's cover say no, and want nothing animating behind them. Said as
        /// intent, because the rasterizer answers it with its cull masks and the ray tracer by not
        /// walking the scene.
        virtual void showWorld(bool shown) = 0;

        /// Turns the world off and back on, and says which it now is: the `tws` console command. A
        /// second reason apart from `showWorld`, because a loading screen that ends while `tws` is
        /// off must not bring the world back. Asked here rather than by editing a cull mask, which
        /// the renderer that culls nothing could not hear said.
        virtual bool toggleWorld() = 0;

        /// The shader chain over the frame, or null where this renderer has none. Owned here,
        /// because what happens between the scene and the screen is the whole of what a renderer is
        /// for; every reader already treats null as "no chain".
        virtual PostProcessor* getPostProcessor() { return nullptr; }

        /// The one point in the frame where the world is the calling thread's alone, for a renderer
        /// that has a schedule to run against it: `MWRender::Session` teleports, aims a camera and
        /// turns a sky, and each is a change to the simulation. Called from `Engine::frame` and from
        /// nowhere else, because a loading screen drives `advance` and `updateTraversal` for frames
        /// of its own and a teleport made from inside one re-enters it.
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

        /// The world, and the GUI over it. Once per frame, from the main loop.
        virtual void renderFrame(const SceneFrame& frame) = 0;

        /// The world under the camera has been replaced rather than moved through, which a ray
        /// tracer reconstructing across frames needs telling and a rasterizer does not.
        virtual void notifyWorldSpaceChanged() {}

        /// A picture of part of the world made somewhere other than the eye, for the GUI to show:
        /// the inventory doll, the race preview, a local map tile. What goes in the picture arrives
        /// in the spec; how it is drawn is the renderer's, which hands back a `MyGUI::ITexture` so
        /// nothing above this line knows which.
        virtual std::unique_ptr<OffscreenView> createOffscreenView(const OffscreenViewSpec& spec) = 0;

        /// The frame the player was last looking at, held still for the GUI, and taken again from
        /// the next frame drawn every time this is called. Whatever the renderer already has rather
        /// than a copy read back to main memory on the frame a load begins. The bottom row of the
        /// picture is texel row nought, as `createOffscreenView` promises and `LoadingScreen`
        /// inverts V for; a renderer whose frame arrives the other way up owes the flip here.
        virtual MyGUI::ITexture& freezeFrame() = 0;

        /// The GUI with no world behind it: the loading screen, a modal message box, a video and
        /// the screenshot all draw a frame from inside another one, and none has a world to
        /// describe.
        virtual void renderGui() = 0;

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

        /// How hard the upscaler between the trace and the picture works, as `RTX / upscale` names
        /// it. Nothing by default, because a rasterizer has no upscaler; a name a renderer cannot
        /// read, or a mode this machine cannot reach, is reported and left where it was, because
        /// what asks is somebody choosing from a menu.
        virtual void setUpscale(std::string_view name) {}

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

        /// MyGUI's backend, a second implementation of MyGUI's own interface. `shaders` is passed in
        /// because this is called at the main menu, before there is a world to reach it through.
        virtual std::unique_ptr<MyGUIPlatform::Platform> createGuiPlatform(osg::Group& guiRoot,
            Resource::ImageManager& images, Shader::ShaderManager& shaders, const VFS::Manager& vfs,
            float scalingFactor, VFS::Path::NormalizedView resourcePath, const std::filesystem::path& logPath)
            = 0;

    protected:
        Renderer() = default;

        /// Taken from whatever made them, once, before anything asks. The ray tracer has no event
        /// queue, because what SDL would put in one is read by `osgViewer` handlers it has not got.
        void adopt(osg::Camera& camera, osg::FrameStamp& frameStamp, osgGA::EventQueue* events, osg::Stats& stats);

        /// Parents the root where this renderer's traversals start from: the viewer's scene data,
        /// or the camera the ray tracer walks from.
        virtual void adoptSceneRoot(osg::Group& root) = 0;

        SceneUtil::AsyncScreenCaptureOperation& getScreenshotWriter() const;

    private:
        osg::ref_ptr<SceneUtil::AsyncScreenCaptureOperation> mScreenshotWriter;
        osg::ref_ptr<osg::Camera> mCamera;
        osg::ref_ptr<osg::FrameStamp> mFrameStamp;
        osg::ref_ptr<osgGA::EventQueue> mEvents;
        osg::ref_ptr<osg::Stats> mStats;
        osg::ref_ptr<osg::Group> mSceneRoot;
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
