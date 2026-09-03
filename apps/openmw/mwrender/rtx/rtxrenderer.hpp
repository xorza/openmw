#ifndef GAME_RENDER_RTX_RTXRENDERER_H
#define GAME_RENDER_RTX_RTXRENDERER_H

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include <osg/ref_ptr>

#include <components/myguiplatform/picture.hpp>
#include <components/rtx/frameimage.hpp>

#include "../renderer.hpp"

#include "bench.hpp"
#include "framecapture.hpp"
#include "worldmirror.hpp"

namespace Resource
{
    class ResourceSystem;
}

namespace osg
{
    class Camera;
    class FrameStamp;
    class Stats;
}

namespace osgGA
{
    class EventQueue;
}

namespace Rtx
{
    class PoseUpdate;
    class Renderer;
}

namespace SceneUtil
{
    class AsyncScreenCaptureOperation;
}

namespace MWRender
{
    /// The picture as rays find it: a window, a mirror of the scene graph, and a trace.
    ///
    /// **It names no graphics API, and there is no `metal/` beside it.** Which API traces is settled
    /// a layer down — `Rtx::createRenderer` picks `components/rtxvulkan` or `components/rtxmetal` by
    /// what the build is for — so everything here is written once for both: the mirror, the frame,
    /// the extents, the capabilities. A second copy of this directory is exactly the duplication
    /// the two-backend split exists to prevent. `mwrender/gl/` is asymmetric with it for a reason:
    /// that renderer *is* an API, down to its sky and its water.
    ///
    /// **No OpenGL is initialised anywhere under this.** No GL context, no `osgViewer` graphics
    /// window, no interop and no rasterized frame underneath — the window is an SDL surface the
    /// backend builds its own surface on, and what reaches the screen is what the trace wrote.
    ///
    /// **It drives the frame itself.** `advance`, `eventTraversal` and `updateTraversal` are
    /// `osgViewer::Viewer`'s, and each is scene-graph work with a graphics context bolted to the
    /// side; what is here is the first half of each and nothing else. There is no cull: rays go
    /// everywhere, so a frustum has nothing to say about what must be reachable — which is also
    /// why the frame is not one late the way an interop path would be. The mirror runs
    /// after the update traversal and the present runs after the mirror, all inside one frame.
    class TracedView;

    class RtxRenderer final : public Renderer
    {
    public:
        /// Throws `std::runtime_error` naming what stopped it — no loader for the backend's API,
        /// no device that qualifies, an upscale mode this build cannot provide. Never falls back: a renderer that
        /// quietly became a different one answers "why does it look like that" with silence.
        explicit RtxRenderer(const RendererSpec& spec);
        ~RtxRenderer() override;

        int getMaxTextureUnits() const override { return mMaxTextureUnits; }

        /// Always. A trace has no frustum to cull against, so how much world exists is the whole
        /// question — and `TerrainGrid` answers it with the cells the simulation happens to hold.
        bool wantsPagedTerrain() const override;

        /// Never, because this path initialises no OpenGL at all: a composite map is a render
        /// target, and `Rtx::TerrainComposite` bakes the flattened texture on the CPU instead.
        float getTerrainCompositeMapLevel() const override;

        /// The distant land radius, which is also what the fog is built to. `cameraDistance` and
        /// `fov` are a frustum's answer and no ray has one.
        float getTerrainViewDistance(float cameraDistance, float fov) const override;
        SDL_Window* getWindow() const override { return mWindow; }

        void attachWorld(RenderingManager& world, osg::Group& worldRoot) override;
        void setSceneRoot(osg::Group& root) override;
        void showWorld(bool shown) override { mWorldShown = shown; }
        bool toggleWorld() override { return mWorldToggled = !mWorldToggled; }

        void advance(double simulationTime) override;
        void eventTraversal() override;
        void updateTraversal() override;

        void renderFrame(const SceneFrame& frame) override;

        void notifyWorldSpaceChanged() override;

        /// **A trace into a texture the GUI already draws from**, at the size asked for and from
        /// the viewpoint handed over: the inventory doll, the race preview, a map tile. A picture of
        /// the world traces against the scene this renderer already holds; a picture of a subject
        /// that stands in no cell is mirrored into a scene of its own.
        std::unique_ptr<OffscreenView> createOffscreenView(const OffscreenViewSpec& spec) override;

        /// **The frame just presented, read back into a GUI texture**, which the loading screen puts
        /// up as its backdrop. One black texel before anything has been presented, which is the very
        /// first load.
        MyGUI::ITexture& freezeFrame() override;

        void renderGui() override;

        bool done() const override { return false; }

        void capture(osg::Image& image, int width, int height) override;
        void saveScreenshot() override;

        /// Nothing draws on another thread, so there is nothing to hold still.
        void suspendDraw() override {}
        void resumeDraw() override {}

        /// No OpenGL objects exist to compile over several frames, and `LoadingScreen` already
        /// reads null as "there is no such thing here".
        osgUtil::IncrementalCompileOperation* getCompileOperation() const override { return nullptr; }
        void setCompileOperation(osgUtil::IncrementalCompileOperation* operation) override {}

        /// **A present mode, which is what a swapchain calls this.** Off is mailbox rather than
        /// immediate — the newest frame and no tearing — and adaptive is relaxed FIFO. Costs a
        /// swapchain rebuild where it changes anything, so the settings window is the only caller.
        void setVSync(SDLUtil::VSyncMode mode) override;

        /// GLSL is the rasterizer's language. What this renderer draws with is compiled SPIR-V, and
        /// swapping it under a running frame is not a thing it offers.
        void reloadChangedShaders(Shader::ShaderManager& shaders) override {}

        std::unique_ptr<MyGUIPlatform::Platform> createGuiPlatform(osg::Group& guiRoot, Resource::ImageManager& images,
            Shader::ShaderManager& shaders, const VFS::Manager& vfs, float scalingFactor,
            VFS::Path::NormalizedView resourcePath, const std::filesystem::path& logPath) override;

        osg::Timer_t getStartTick() const override { return mStartTick; }

        /// The OSG stats overlay is the rasterizer's instrumentation and the rasterizer draws it.
        /// What this renderer has instead is its own frame times and `OPENMW_RTX_BENCH`.
        void installStatsOverlay(const VFS::Manager& vfs, bool toFile) override {}
        void reportStats(unsigned frameNumber, std::ostream& stream) const override {}

        /*internal:*/

        /// Whether the world has reached the backend yet, so a picture traced against it would be a
        /// picture of something.
        bool hasScene() const { return mHasScene; }

        /// Draws `view` on the next frame that has a world in it.
        ///
        /// **A cell asks for its map tile as it loads**, which is the frame before the one that
        /// first mirrors it. Without this the tile the player starts on stays blank until a
        /// neighbour arriving makes the local map ask for it again.
        void deferRedraw(TracedView& view);

        /// Takes a view off that list, because it is going away.
        void forgetView(TracedView& view);

        /// The one sequence every mirror walk here poses at — the world's, and every traced view's.
        ///
        /// **Shared rather than each keeping its own**, because a subtree both can reach would
        /// otherwise be posed by whichever counter got there first and frozen for the other.
        /// See `Rtx::Traversals`.
        Rtx::Traversals& getTraversals() { return mMirror.getTraversals(); }

        /// The game's frame number, which is which of a `SceneUtil::LightSource`'s two buffers
        /// update has just written. Not a pose number; see `getTraversals`.
        std::size_t getFrame() const { return mFrame; }

        /// Where a picture of its own subject gets its textures from. Null before there is a world.
        Resource::ResourceSystem* getResources() const { return mResources; }

        /// The clock an update traversal runs on: this renderer's own, which advances once per
        /// drawn frame whether or not the world's does.
        ///
        /// **What a picture of its own subject is posed against.** The rasterizer hangs an offscreen
        /// view's camera off the scene graph, so the viewer's update traversal reaches the subtree
        /// under it; there is no such graph here, and `Rtx::OffscreenTrace` runs the traversal
        /// itself against this. Not `getFrame`, which stops when the game is paused — a doll posed
        /// against a stopped clock is a doll frozen the first time it was drawn.
        const osg::FrameStamp& getUpdateStamp() const { return *mFrameStamp; }

    private:
        /// Makes the SDL window the backend builds its surface on. No GL attribute is set and no GL
        /// flag is passed, which is what `SDL_GL_GetCurrentContext() == nullptr` then proves.
        void createWindow(const std::filesystem::path& resourceDir);

        /// Resizes the trace to the window where the window has changed under it.
        void fitToWindow();

        /// Traces the world the walk has just mirrored, from the eye the frame arrived with.
        ///
        /// **Its refusals are not the frame's.** A world with nothing in it and a camera with no
        /// roll are both reasons not to trace and neither is a reason not to present, so they end
        /// here rather than in `renderFrame` — see the comment on the call.
        ///
        /// `walkMs` is what the mirror took, carried through rather than measured here: the
        /// benchmark's row is closed at the end of the trace and the walk is over before it starts.
        ///
        /// @return whether anything was written into the target.
        bool traceWorld(const SceneFrame& frame, const Rtx::ExtractionStats& found, double walkMs);

        /// Hands MyGUI's triangles to the renderer, where there is a GUI up at all.
        void drawGui();

        /// Draws whatever asked before there was a world to draw it against.
        void drawDeferredViews();

        Stage& mStage;
        int mMaxTextureUnits = 0;

        /// Whether the world has been handed to the backend at least once.
        bool mHasScene = false;

        /// Whether a screen is over the world. False behind a loading screen and the main menu's
        /// cover, where the walk would read a world nothing is updating. `Renderer::showWorld`.
        bool mWorldShown = true;

        /// Whether the player asked to see the world at all. The `tws` console command, and a
        /// second answer rather than the same one: a loading screen that ends while `tws` is off
        /// must not bring the world back. `Renderer::toggleWorld`.
        bool mWorldToggled = true;

        /// Whether this frame has a world in it, which is both of the answers above and nothing
        /// else. Said once, because a frame that walked on one of them and traced on the other
        /// would mirror a world it then threw away.
        bool drawsWorld() const { return mWorldShown && mWorldToggled; }

        /// The world's, for a picture that has to resolve textures of its own. Null until
        /// `attachWorld`.
        Resource::ResourceSystem* mResources = nullptr;

        /// Pictures that asked to be drawn before it had. Raw pointers because the caller owns
        /// every view; `forgetView` is what keeps that sound.
        std::vector<TracedView*> mDeferred;

        /// The list a flush walks, swapped out of `mDeferred` so a redraw cannot grow what is being
        /// iterated. Kept rather than made, because this sits on the frame path.
        std::vector<TracedView*> mDrawing;

        MyGUIPlatform::Picture mFrozenFrame{ "frozen frame" };

        /// Screenshots, savegame thumbnails and the frames `OPENMW_RTX_SHOT` writes.
        ///
        /// The screenshot writer inside it is the same one the OpenGL renderer uses, so the two
        /// write the same file the same way.
        FrameCapture mCapture;

        SDL_Window* mWindow = nullptr;

        /// What the stage was handed. Made here because there is no viewer to make them, and held
        /// because the frame is driven from them.
        osg::ref_ptr<osg::Camera> mCamera;
        osg::ref_ptr<osg::FrameStamp> mFrameStamp;
        osg::ref_ptr<osgGA::EventQueue> mEvents;
        osg::ref_ptr<Rtx::PoseUpdate> mUpdateVisitor;
        osg::ref_ptr<osg::Stats> mStats;
        osg::ref_ptr<osg::Group> mSceneRoot;

        /// Where `advance` measures reference time from, and the origin the profiler's spans are
        /// stamped against.
        osg::Timer_t mStartTick = 0;

        std::unique_ptr<Rtx::Renderer> mRenderer;

        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;

        /// The one sequence every mirror walk in this renderer poses at.
        ///
        /// **Before the extractors, because they hold a reference to it.** Shared with every
        /// `TracedView`, which is the whole point: a subtree the world and a doll can both reach must
        /// The engine's scene graph mirrored into what a ray can meet, and the hand-over that
        /// puts it on the device.
        WorldMirror mMirror;

        /// A running average of what the trace costs, reported every `sReportEvery` frames.
        ///
        /// **The only instrument on this path.** The harness times a frame by tracing it thirty
        /// times and taking the best; a game cannot, so what it can say is what the last few hundred
        /// frames came to on average — which is the number that matters when the question is whether
        /// this is playable.
        double mSpentMs = 0.0;
        std::uint32_t mTimed = 0;

        /// Times a run of frames when asked to, and is not compiled at all when it cannot be.
        Bench mBench;

        /// When the last frame was handed over, so what `Bench` measures is the whole frame and not
        /// this renderer's slice of it.
        std::chrono::steady_clock::time_point mEntered;
        bool mEnteredOnce = false;

        /// The frame number the walk and the trace are both stamped with, so what the upscaler
        /// jitters and what the sampler walks are the same sequence the world is counting.
        std::size_t mFrame = 0;

        /// Whether a camera the builder refused has already been reported. `traceWorld` says why
        /// once is the whole of it.
        bool mComplained = false;
    };
}

#endif
