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
#include <components/rtx/upscale.hpp>
#include <components/sdlutil/vsyncmode.hpp>
#include <components/settings/values.hpp>
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
    class UpdateVisitor;
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
    class WorkQueue;
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

    /// What every renderer needs to exist, whatever it draws with.
    struct RtxSetup;

    /// What a renderer may spend per frame on preparing what a loader handed it.
    struct PreparationBudget
    {
        double mSecondsPerFrame = 0.0;
        unsigned int mObjectsPerFrame = 0;
    };

    /// How the game builds the world's ground for this renderer.
    ///
    /// **A policy and not a capability.** Each of these decides what the game *builds*, and a
    /// renderer that answered three of them consistently and the fourth by accident builds ground
    /// nobody draws — which is why they are one struct and not four questions.
    ///
    /// **Fixed for a renderer's life and asked once per worldspace.** `getTerrainViewDistance` is
    /// deliberately not here: it is a live function of the camera distance and the field of view for
    /// the renderer that widens by them, and a value asked once could not answer it.
    struct TerrainPlan
    {
        /// Whether the world's ground is paged into a quad tree rather than built a cell at a time.
        ///
        /// **Not a preference where rays are what reach it.** `TerrainGrid` makes the cells the
        /// simulation has loaded and nothing else, so a renderer answering no has no distant land at
        /// any radius — which is why this is the renderer's answer and not `distant terrain`'s,
        /// whose default is off and whose business is what a rasterizer can afford to draw.
        bool mPaged = false;

        /// Whether the game builds the ground's chunks for this renderer at all.
        ///
        /// **A renderer that stands the ground itself answers no**, and the world it is given holds
        /// the storage, the worldspace and the active grid and builds nothing: `Rtx::CellRing` reads
        /// every cell's heights and blend maps off `Terrain::Storage` on a thread of its own, so a
        /// quad tree beside it would build chunks nobody draws — on the work thread, and inside
        /// `Scene::changeCellGrid`'s synchronous wait for them.
        bool mChunks = true;

        /// Whether the statics of the distance are merged into those chunks by
        /// `Terrain::ObjectPaging`.
        ///
        /// **The setting's answer for a rasterizer, and a renderer's own where it stands the
        /// distance itself.** The paging exists to turn a thousand draw calls into a few; a ray
        /// tracer instances a thousand copies of one model for the price of one, and what a merged
        /// chunk costs it is the fold of the merge on the frame it arrives. `Rtx::CellRing` is that
        /// renderer's answer, and it reads the same setting as its own switch.
        bool mObjectPaging = false;

        /// Chunk size, in cells, past which the terrain flattens a chunk's layer stack into one
        /// composite map — or `Terrain::sNoCompositeMap` for a renderer that will never ask for one.
        ///
        /// **A composite map is a render target**, which is a thing a renderer either draws into or
        /// has no way to make. Asking the terrain to build one for a renderer that cannot is how a
        /// chunk ends up carrying a texture nothing can open.
        float mCompositeMapLevel = 0.0f;
    };

    struct RendererSpec
    {
        /// For work a frame must not wait on — writing a screenshot to disk, so far.
        SceneUtil::WorkQueue& mWorkQueue;

        /// Where the window icon is read from.
        std::filesystem::path mResourceDir;

        std::filesystem::path mScreenshotPath;

        /// Where a renderer keeps what it compiled — `ConfigurationManager::getCachePath`. What goes
        /// there is regenerable, so it is the cache directory and not the user's data.
        std::filesystem::path mCachePath;

        /// What a harness run asks of the ray tracer, or null. `GlRenderer` ignores it.
        const RtxSetup* mRtx = nullptr;
    };

    /// One image of the world on the screen, and the window it goes in.
    ///
    /// **Nothing below this line is abstracted.** Contexts, swapchains, command buffers,
    /// framebuffers, render bins, descriptor sets and acceleration structures belong to a renderer
    /// outright — an interface over those would be a mini-GL that Vulkan does not fit, which is the
    /// argument the backend boundary makes one level further down for the same reason. What is
    /// here is what the game asks for, and none of it is called more than a few times a frame.
    ///
    /// **A pure virtual is a question both renderers answer. A question only the rasterizer can
    /// answer has a default here**, and the ray tracer does not override it: an override saying
    /// nothing is a place a reader has to check to learn that the question was the rasterizer's.
    class Renderer
    {
    public:
        virtual ~Renderer();

        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        /// How many textures one shader may sample. `Shader::ShaderManager` reserves its global
        /// units out of this, and content that wants one more than there are has to be told rather
        /// than find out at link time.
        ///
        /// **A GL fact, so the default is the one number a renderer with no GL context can give**:
        /// `sAssumedTextureUnits`, which is what the content was described against.
        virtual int getMaxTextureUnits() const { return sAssumedTextureUnits; }

        /// What the shader visitor is told a GPU offers, for a host that has no GL context to ask.
        ///
        /// **A stand-in and not a capability.** The visitor runs on every model OpenMW loads and
        /// needs a number to fit texture slots into. The value decides only how many slots it is
        /// willing to use; the roles it labels them with, which is the whole of what a described
        /// surface carries, are the same for any number this large.
        static constexpr int sAssumedTextureUnits = 32;

        /// The window the renderer made. Input, the GUI's scale and the gamma ramp are SDL's
        /// business and read it; what is bound to it is not theirs to know.
        virtual SDL_Window* getWindow() const = 0;

        /// How the game builds the world's ground for this renderer. `TerrainPlan` says what each
        /// answer decides, and why the view distance below is not one of them.
        virtual TerrainPlan getTerrainPlan() const = 0;

        /// What the game says of one reference the content files cannot: a script has disabled it,
        /// or enabled it again. For a renderer standing the distance itself; the paging is told by
        /// its own route.
        virtual void enableReference(ESM::RefNum refnum, bool enabled) {}

        /// The world is going. Anything of it a renderer reads from a thread of its own has to be
        /// let go of before it does, and `attachWorld` had no counterpart to say so.
        virtual void detachWorld() {}

        /// How far from the eye ground is made at all, in units.
        ///
        /// **A frustum's reach and a radius are different questions**, and only the renderer knows
        /// which one it is asking. What is culled needs the corners of the frustum covered, so
        /// `cameraDistance` is widened by `fov`; what is traced needs to know how much world exists,
        /// which is a property of the structure rays are cast against and of no camera at all.
        virtual float getTerrainViewDistance(float cameraDistance, float fov) const = 0;

        /// How far from the eye ground is *built*, in units, straight ahead and not to a frustum's
        /// corner: what the local map is a map of. Nought where the ground reaches no further than
        /// the cells the simulation has loaded.
        virtual float getGroundReach() const = 0;

        /// The world exists; build whatever goes between it and the screen.
        ///
        /// **A second phase because it cannot be a first one.** The renderer is made before there is
        /// a world — the window has to exist before anything can be loaded into it — and what the
        /// rasterizer puts in front of the world needs the world to talk to. So the frame graph is
        /// built here, along with whatever else a renderer wants above the scene, and this is also
        /// where the stage is told what is topmost.
        ///
        /// Called once, from `RenderingManager`'s constructor.
        virtual void attachWorld(RenderingManager& world, osg::Group& worldRoot) = 0;

        /// Whatever the renderer wants culled and drawn. Not always the node the world was built
        /// under: the rasterizer wraps it in its post-processing group and hands back the wrapper.
        ///
        /// **The root hangs off the camera, and that is what lets the player touch the world.**
        /// `RenderingManager::castCameraToViewportRay` accepts an intersection visitor on the camera
        /// because its ray is in projection coordinates and the camera's matrices are what put that
        /// ray in the world. Parented once however often it is said: the rasterizer parents the
        /// same root a second time through `osgViewer::Viewer`.
        void setSceneRoot(osg::Group& root);

        /// The camera, the frame stamp, the input queue and the stats, which the game reads whatever
        /// draws. Adopted by the renderer that made them — `osgViewer` builds the rasterizer's, the
        /// ray tracer builds its own — and asked of the renderer rather than of a second object
        /// standing beside it.
        osg::Camera& getCamera() const;
        osg::FrameStamp& getFrameStamp() const;
        osgGA::EventQueue* getEvents() const { return mEvents.get(); }
        osg::Stats& getStats() const;

        osg::Group& getSceneRoot() const;
        bool hasSceneRoot() const { return mSceneRoot != nullptr; }

        /// Whether the world is being shown at all. The interface is drawn either way.
        ///
        /// **A loading screen and the main menu's cover are the two that say no**, and what they
        /// want is a frame that is the interface and nothing else: nothing animating behind it, and
        /// nothing drawn that the interface is about to cover.
        ///
        /// **Said as intent, because the two renderers have nothing to share here.** The rasterizer
        /// answers it by blanking its update traversal and culling to the GUI's masks; this one has
        /// no cull at all and answers by not walking the scene and not tracing it.
        virtual void showWorld(bool shown) = 0;

        /// Turns the world off and back on, and says which it now is. The `tws` console command.
        ///
        /// **A second and independent reason, and not the one `showWorld` carries.** That one says
        /// whether a screen is over the world right now; this one says whether the player asked to
        /// see the world at all, and a loading screen that ends while `tws` is off must not bring it
        /// back. The world is drawn when both say so.
        ///
        /// **Asked here rather than by editing a cull mask.** A mask is the rasterizer's vocabulary,
        /// and the renderer that culls nothing had no way to hear it said: `tws` moved the water and
        /// left every other thing in the world traced, which is worse than a toggle that does
        /// nothing at all.
        ///
        /// **The two renderers leave different pictures behind, and that is not hidden.** The
        /// rasterizer drops the five categories `SceneUtil::sToggleWorldMask` names and keeps its
        /// sky and its sea over the emptiness; the ray tracer stops tracing, so what is left is the
        /// interface over nothing. Matching the first would mean taking those categories out of the
        /// mirror and letting the next sweep retire them, which is a larger change than a debug
        /// command has yet asked for.
        virtual bool toggleWorld() = 0;

        /// The shader chain over the frame, or null where this renderer has none.
        ///
        /// **Owned here and not by the world.** It is a renderer's answer to "what happens between
        /// the scene and the screen", which is the whole of what a renderer is for; a world that
        /// held one would have to know whether the renderer it was given wanted it. Eleven Lua
        /// bindings, the HUD and the settings page read this and already treat null as "no chain".
        virtual PostProcessor* getPostProcessor() { return nullptr; }

        /// The one point in the frame where the world is the calling thread's alone, offered to a
        /// renderer that has a schedule to run against it.
        ///
        /// **Nothing by default, because only a renderer that is being driven has anything to do
        /// here.** `MWRender::Session` teleports, aims a camera and turns a sky, and each of those
        /// is a change to the simulation rather than to a picture — so it has to happen where the
        /// simulation is changed, before the scripts, the mechanics and the Lua worker.
        ///
        /// **Not `advance` or `updateTraversal`, which a loading screen drives frames of its own
        /// through.** A teleport made from either re-enters the call it was made from once per
        /// loaded cell, and measured, that hangs: the ring reads, the screen ticks, and
        /// `changeToCell` never returns. This one is called from `Engine::frame` and from nowhere
        /// else, so a screen that draws under it cannot reach it again.
        virtual void tickSchedule() {}

        /// Opens the frame's clock and says how long the frame stands for, in seconds.
        ///
        /// **A renderer that has to repeat itself keeps the clock**, because how far the simulation
        /// steps, how long the renderer is told the frame took and what the caches age by have to be
        /// one number — `Rtx::FrameClock` says what three of them cost. The default is the
        /// measurement it was handed, which is what the loop worked out for itself before.
        ///
        /// @param measured what the wall says the last frame took, in seconds.
        virtual double beginFrame(double measured) { return measured; }

        /// Stamps the next frame. Simulation time stops when the game is paused; reference time
        /// does not.
        virtual void advance(double simulationTime) = 0;

        virtual void eventTraversal() = 0;
        virtual void updateTraversal() = 0;

        /// The world, and the GUI over it. Once per frame, from the main loop.
        virtual void renderFrame(const SceneFrame& frame) = 0;

        /// The world under the camera has been replaced rather than moved through.
        ///
        /// **A ray tracer reconstructing across frames needs telling, and a rasterizer does not** —
        /// which is why this has a body rather than being pure. Upstream's renderer draws each frame
        /// from nothing and has no history for a teleport to invalidate; the traced path accumulates
        /// one, and a cell load or a door leaves the camera somewhere its previous basis describes
        /// nothing about.
        virtual void notifyWorldSpaceChanged() {}

        /// A picture of part of the world made somewhere other than the eye, for the GUI to show:
        /// the inventory doll, the race preview, a local map tile.
        ///
        /// **The renderer makes it because the renderer is what will draw it**, and it hands back a
        /// `MyGUI::ITexture` so that nothing above this line has to know which one it got. What goes
        /// in the picture is the game's and arrives in the spec; how it is drawn is not described
        /// there at all.
        virtual std::unique_ptr<OffscreenView> createOffscreenView(const OffscreenViewSpec& spec) = 0;

        /// The frame the player was last looking at, held still, as a picture for the GUI to show.
        /// Taken again from the next frame drawn, every time this is called.
        ///
        /// **Whatever the renderer already has, rather than a copy the GUI makes.** The rasterizer
        /// copies its own framebuffer where it stands; a renderer that owns its swapchain has the
        /// image it just presented. Reading either back to main memory and handing it over as pixels
        /// would be the same picture at several times the price, on the frame a load begins.
        ///
        /// **The bottom row of the picture is texel row nought**, which is what `createOffscreenView`
        /// promises of its own texture and what `LoadingScreen` inverts the widget's V for. OpenGL
        /// puts a framebuffer that way round and the GUI was written against it; a renderer whose
        /// frame arrives the other way up owes the flip here. Unwritten, that cost the ray tracer a
        /// world stood on its head behind every progress bar.
        virtual MyGUI::ITexture& freezeFrame() = 0;

        /// The GUI, with no world behind it.
        ///
        /// **The four places that get a frame onto the screen from inside another one** — the
        /// loading screen, a modal message box, a video and the screenshot — and none of them has a
        /// world to describe. A renderer that culls cannot tell the two apart and answers both the
        /// same way; one that mirrors the graph and traces it very much can.
        virtual void renderGui() = 0;

        /// Whether the window has been closed. A renderer whose window is closed by SDL's own quit
        /// event answers no.
        virtual bool done() const { return false; }

        /// The frame without the GUI, into an image. The screenshot console command and the save
        /// thumbnails; blocks until the frame it asked for has been drawn.
        virtual void capture(osg::Image& image, int width, int height) = 0;

        /// The screenshot key, which writes a file rather than handing back an image.
        virtual void saveScreenshot() = 0;

        /// Between these two nothing is reading the scene graph, so it can be mutated. A renderer
        /// that draws on the calling thread has nothing to hold still.
        virtual void suspendDraw() {}
        virtual void resumeDraw() {}

        /// Compiles arriving resources over several frames instead of stalling on first use. Null
        /// where `OPENMW_DONT_PRECOMPILE` asked for none, which is why this one is a pointer.
        /// **A budget and not a compile operation**, because that operation is an OpenGL object and
        /// the interface would otherwise hold one and the ray tracer answer with null. What the
        /// screen actually asks for is "spend more while I am up", and that is a number.
        ///
        /// `reset` puts back what the first `set` since the last reset found, which is what the
        /// loading screen did for itself upstream. A renderer with nothing to prepare ignores both.
        virtual void setPreparationBudget(const PreparationBudget& budget) {}
        virtual void resetPreparationBudget() {}

        /// The operation an OSG loader compiles through, or null. **Not the frame path's**: this is
        /// what `Resource::SceneManager` and the paging hand their new nodes to.
        virtual osgUtil::IncrementalCompileOperation* getCompileOperation() const { return nullptr; }

        virtual void setVSync(SDLUtil::VSyncMode mode) = 0;

        /// How hard the upscaler between the trace and the picture works.
        ///
        /// **Nothing by default, because a rasterizer has no upscaler to work.** `Rtx::Upscale` is
        /// named here for the reason `SDLUtil::VSyncMode` is: it is the value the setting holds and
        /// the one the backend acts on, and a second spelling in between would be two ideas of what
        /// `quality` means. The header is a handful of inline functions and is there in every build.
        ///
        /// **A mode this machine cannot reach is refused and reported, not thrown**, because what
        /// asks is somebody choosing from a menu. `getUpscale` is what they are actually running.
        virtual void setUpscale(Rtx::Upscale upscale) {}

        /// Which mode the frames are drawn under, or `Off` for a renderer with no upscaler at all.
        virtual Rtx::Upscale getUpscale() const { return Rtx::Upscale::Off; }

        /// Recompiles whatever shader source has been edited since the last call. Costs a directory
        /// scan and nothing else when the feature is off, which is what makes it callable per frame.
        /// GLSL is the rasterizer's language; a renderer drawing with compiled SPIR-V has nothing to
        /// reload.
        virtual void reloadChangedShaders(Shader::ShaderManager& shaders) {}

        /// The origin the per-frame profiler measures from, so its spans land on the same axis as
        /// the counters the renderer writes beside them.
        virtual osg::Timer_t getStartTick() const = 0;

        /// This renderer's own instrumentation — the overlay its debug keys toggle, and the
        /// per-frame dump `OPENMW_OSG_STATS_FILE` asks for. What it counts is its own business, so
        /// what it draws and what it writes are too. The OSG stats overlay is the rasterizer's; a
        /// renderer with its own frame times has nothing to install.
        virtual void installStatsOverlay(const VFS::Manager& vfs, bool toFile) {}
        virtual void reportStats(unsigned frameNumber, std::ostream& stream) const {}

        /// MyGUI's backend. `MyGUI::RenderManager` is MyGUI's own interface, so a second one of
        /// these is a second implementation of an existing interface rather than a new abstraction.
        ///
        /// @param shaders where the rasterizer finds the program it draws widgets with. Passed in
        ///        rather than reached for: this is called at the main menu, where there is no world
        ///        and so nothing a renderer could have been handed one through.
        virtual std::unique_ptr<MyGUIPlatform::Platform> createGuiPlatform(osg::Group& guiRoot,
            Resource::ImageManager& images, Shader::ShaderManager& shaders, const VFS::Manager& vfs,
            float scalingFactor, VFS::Path::NormalizedView resourcePath, const std::filesystem::path& logPath)
            = 0;

    protected:
        Renderer() = default;

        /// Taken from whatever made them, once, before anything asks. The ray tracer has no event
        /// queue: what SDL would put in one is the function keys, which upstream reads with
        /// `osgViewer` handlers it does not have.
        void adopt(osg::Camera& camera, osg::FrameStamp& frameStamp, osgGA::EventQueue* events, osg::Stats& stats);

        /// What a renderer does with the root beyond hanging it off the camera: the rasterizer hands
        /// it to its viewer.
        virtual void adoptSceneRoot(osg::Group& root) {}

    private:
        osg::ref_ptr<osg::Camera> mCamera;
        osg::ref_ptr<osg::FrameStamp> mFrameStamp;
        osg::ref_ptr<osgGA::EventQueue> mEvents;
        osg::ref_ptr<osg::Stats> mStats;
        osg::ref_ptr<osg::Group> mSceneRoot;
    };

    /// Runs the camera's own update callback and nothing below it.
    ///
    /// **The eye is updated without the world being walked a second time.** A renderer that drives
    /// its own frame walks the scene from its own root — for the node path, which must not start at
    /// an `ABSOLUTE_RF` camera — and then wants the one callback the camera carries. Accepting on
    /// the camera to get it ran every animation controller, every `LightController` and
    /// `LightManager::update` twice in the same frame, at the same traversal number.
    ///
    /// The callback belongs to `MWRender::Camera` — attached in its constructor, removed in its
    /// destructor — so a camera carrying none is a frame outside that object's life, and nothing
    /// happens.
    void updateEye(osg::Camera& camera, osgUtil::UpdateVisitor& visitor);

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

    /// What every renderer asks SDL for, and the hints it has to have set before asking.
    ///
    /// **Everything here is the video settings and none of it is a graphics API.** Screen, size,
    /// window mode, border, what happens on focus loss — two renderers wanted the same twenty lines
    /// and the only difference between them is one flag naming what will be drawn into the surface.
    ///
    /// **The hints are set here because this is the last moment they can be.** SDL reads them inside
    /// `SDL_CreateWindow`, so a caller that set them afterwards would be setting them for the next
    /// window.
    ///
    /// @param surfaceFlag what the window is for: `SDL_WINDOW_OPENGL` for the rasterizer, and
    ///        whatever surface the other renderer's backend asks for.
    WindowPlacement describeWindow(std::uint32_t surfaceFlag);

    /// Reads `openmw.png` from beside the resources and gives it to the window.
    ///
    /// Logs and carries on wherever it cannot: a window with no icon is still a window, and this
    /// runs before there is anything on screen to report a failure with.
    void setWindowIcon(SDL_Window& window, const std::filesystem::path& resourceDir);

    /// The writer both renderers hand a captured frame to.
    ///
    /// **Shared rather than one apiece**, so the two write the same files to the same place with the
    /// same names and say the same thing afterwards. Where the picture came from — a frame buffer or
    /// a trace — is the renderer's business; the format, the path and the message are the game's.
    osg::ref_ptr<SceneUtil::AsyncScreenCaptureOperation> makeScreenshotWriter(
        SceneUtil::WorkQueue& queue, const std::filesystem::path& screenshotPath);
}

#endif
