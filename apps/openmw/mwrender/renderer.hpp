#ifndef GAME_RENDER_RENDERER_H
#define GAME_RENDER_RENDERER_H

#include <filesystem>
#include <iosfwd>
#include <memory>
#include <string_view>

#include <osg/Timer>

#include <components/sdlutil/vsyncmode.hpp>
#include <components/vfs/pathutil.hpp>

struct SDL_Window;

namespace osg
{
    class Group;
    class Image;
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
    class Stage;

    /// What every renderer needs to exist, whatever it draws with.
    struct RendererSpec
    {
        /// The frame, the eye and the input queue. Made before any renderer and outliving it: the
        /// renderer adopts these objects rather than making its own, which is what lets the game
        /// hold the camera and the frame stamp without knowing what draws.
        Stage& mStage;

        /// For work a frame must not wait on — writing a screenshot to disk, so far.
        SceneUtil::WorkQueue& mWorkQueue;

        /// Where the window icon is read from.
        std::filesystem::path mResourceDir;

        std::filesystem::path mScreenshotPath;
    };

    /// One image of the world on the screen, and the window it goes in.
    ///
    /// **Nothing below this line is abstracted.** Contexts, swapchains, command buffers,
    /// framebuffers, render bins, descriptor sets and acceleration structures belong to a renderer
    /// outright — an interface over those would be a mini-GL that Vulkan does not fit, which is the
    /// argument the backend boundary makes one level further down for the same reason. What is
    /// here is what the game asks for, and none of it is called more than a few times a frame.
    class Renderer
    {
    public:
        virtual ~Renderer() = default;

        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        /// How many textures one shader may sample. `Shader::ShaderManager` reserves its global
        /// units out of this, and content that wants one more than there are has to be told rather
        /// than find out at link time.
        ///
        /// **The only thing a renderer is asked what it has, and it is a number.** There was a
        /// `Capabilities` struct here and every other field in it turned out to be a null check
        /// wearing a hat: `getPostProcessor()` returning nothing says the same as a
        /// `mPostProcessing` that is false, and says it without putting the shape of one renderer's
        /// insides in the caller. Asking a renderer for the thing is a better question than asking
        /// it what it has — except where the answer is not a thing.
        virtual int getMaxTextureUnits() const = 0;

        /// The window the renderer made. Input, the GUI's scale and the gamma ramp are SDL's
        /// business and read it; what is bound to it is not theirs to know.
        virtual SDL_Window* getWindow() const = 0;

        /// Whether the world's ground is paged into a quad tree rather than built a cell at a time.
        ///
        /// **Not a preference where rays are what reach it.** `TerrainGrid` makes the cells the
        /// simulation has loaded and nothing else, so a renderer answering no has no distant land at
        /// any radius — which is why this is asked of the renderer and not read from `distant
        /// terrain`, whose default is off and whose business is what a rasterizer can afford to draw.
        virtual bool wantsPagedTerrain() const = 0;

        /// Chunk size, in cells, past which the terrain flattens a chunk's layer stack into one
        /// composite map — or `Terrain::sNoCompositeMap` for a renderer that will never ask for one.
        ///
        /// **A composite map is a render target**, which is a thing a renderer either draws into or
        /// has no way to make. Asking the terrain to build one for a renderer that cannot is how a
        /// chunk ends up carrying a texture nothing can open.
        virtual float getTerrainCompositeMapLevel() const = 0;

        /// How far from the eye ground is made at all, in units.
        ///
        /// **A frustum's reach and a radius are different questions**, and only the renderer knows
        /// which one it is asking. What is culled needs the corners of the frustum covered, so
        /// `cameraDistance` is widened by `fov`; what is traced needs to know how much world exists,
        /// which is a property of the structure rays are cast against and of no camera at all.
        virtual float getTerrainViewDistance(float cameraDistance, float fov) const = 0;

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
        virtual void setSceneRoot(osg::Group& root) = 0;

        /// The shader chain over the frame, or null where this renderer has none.
        ///
        /// **Owned here and not by the world.** It is a renderer's answer to "what happens between
        /// the scene and the screen", which is the whole of what a renderer is for; a world that
        /// held one would have to know whether the renderer it was given wanted it. Eleven Lua
        /// bindings, the HUD and the settings page read this and already treat null as "no chain".
        virtual PostProcessor* getPostProcessor() { return nullptr; }

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
        virtual MyGUI::ITexture& freezeFrame() = 0;

        /// The GUI, with no world behind it.
        ///
        /// **The four places that get a frame onto the screen from inside another one** — the
        /// loading screen, a modal message box, a video and the screenshot — and none of them has a
        /// world to describe. A renderer that culls cannot tell the two apart and answers both the
        /// same way; one that mirrors the graph and traces it very much can.
        virtual void renderGui() = 0;

        /// Whether the window has been closed.
        virtual bool done() const = 0;

        /// The frame without the GUI, into an image. The screenshot console command and the save
        /// thumbnails; blocks until the frame it asked for has been drawn.
        virtual void capture(osg::Image& image, int width, int height) = 0;

        /// The screenshot key, which writes a file rather than handing back an image.
        virtual void saveScreenshot() = 0;

        /// Between these two nothing is reading the scene graph, so it can be mutated. A renderer
        /// that draws on the calling thread answers both with nothing.
        virtual void suspendDraw() = 0;
        virtual void resumeDraw() = 0;

        /// Compiles arriving resources over several frames instead of stalling on first use. Null
        /// where `OPENMW_DONT_PRECOMPILE` asked for none, which is why this one is a pointer.
        virtual osgUtil::IncrementalCompileOperation* getCompileOperation() const = 0;
        virtual void setCompileOperation(osgUtil::IncrementalCompileOperation* operation) = 0;

        virtual void setVSync(SDLUtil::VSyncMode mode) = 0;

        /// Recompiles whatever shader source has been edited since the last call. Costs a directory
        /// scan and nothing else when the feature is off, which is what makes it callable per frame.
        virtual void reloadChangedShaders(Shader::ShaderManager& shaders) = 0;

        /// The origin the per-frame profiler measures from, so its spans land on the same axis as
        /// the counters the renderer writes beside them.
        virtual osg::Timer_t getStartTick() const = 0;

        /// This renderer's own instrumentation — the overlay its debug keys toggle, and the
        /// per-frame dump `OPENMW_OSG_STATS_FILE` asks for. What it counts is its own business, so
        /// what it draws and what it writes are too.
        virtual void installStatsOverlay(const VFS::Manager& vfs, bool toFile) = 0;
        virtual void reportStats(unsigned frameNumber, std::ostream& stream) const = 0;

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
    };

    /// The one place the choice is made. Throws naming the name where there is no such renderer,
    /// because a fallback would answer "why does it look like that" with silence.
    std::unique_ptr<Renderer> createRenderer(std::string_view name, const RendererSpec& spec);
}

#endif
