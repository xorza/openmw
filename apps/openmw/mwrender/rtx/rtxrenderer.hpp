#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include <osg/ref_ptr>

#include <components/myguiplatform/picture.hpp>
#include <components/rtx/frameclock.hpp>
#include <components/rtx/frameimage.hpp>
#include <components/rtx/renderprofile.hpp>

#include "../renderer.hpp"
#include "framecapture.hpp"
#include "session.hpp"
#include "tracedrun.hpp"
#include "viewhost.hpp"
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
    class TracedView;

    /// The picture as rays find it: a window, a mirror of the scene graph, and a trace.
    ///
    /// **It names no graphics API.** Which one traces is settled a layer down, where
    /// `Rtx::createRenderer` hands back what this build has — so the mirror, the frame, the extents
    /// and the capabilities are written against none of it. `mwrender/gl/` is asymmetric with it
    /// for a reason: that renderer *is* an API, down to its sky and its water.
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
    class RtxRenderer final : public Renderer, public ViewHost
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

        void tickSchedule() override;
        double beginFrame(double measured) override;
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

        /// **A present mode, which is what a swapchain calls this.** Off is mailbox rather than
        /// immediate — the newest frame and no tearing — and adaptive is relaxed FIFO. Costs a
        /// swapchain rebuild where it changes anything, so the settings window is the only caller.
        void setVSync(SDLUtil::VSyncMode mode) override;
        void setUpscale(Rtx::Upscale upscale) override;
        Rtx::Upscale getUpscale() const override { return mRenderer->getUpscale(); }

        /// GLSL is the rasterizer's language. What this renderer draws with is compiled SPIR-V, and
        /// swapping it under a running frame is not a thing it offers.
        void reloadChangedShaders(Shader::ShaderManager& shaders) override {}

        std::unique_ptr<MyGUIPlatform::Platform> createGuiPlatform(osg::Group& guiRoot, Resource::ImageManager& images,
            Shader::ShaderManager& shaders, const VFS::Manager& vfs, float scalingFactor,
            VFS::Path::NormalizedView resourcePath, const std::filesystem::path& logPath) override;

        osg::Timer_t getStartTick() const override { return mStartTick; }

        /// The OSG stats overlay is the rasterizer's instrumentation and the rasterizer draws it.
        /// What this renderer has instead is its own frame times and `MWRender::Session`.
        void installStatsOverlay(const VFS::Manager& vfs, bool toFile) override {}
        void reportStats(unsigned frameNumber, std::ostream& stream) const override {}

        /*internal:*/

        /// What a measured stop is allowed to look at. `TracedRun` says why it is a value.
        TracedRun describeRun();

        Rtx::Renderer& getBackend() override { return *mRenderer; }
        bool hasScene() const override { return mHasScene; }
        std::optional<PoseMoment> describePose() override;
        void deferRedraw(TracedView& view) override;
        void forgetView(TracedView& view) override;

    private:
        /// Makes the SDL window the backend builds its surface on. No GL attribute is set and no GL
        /// flag is passed, which is what `SDL_GL_GetCurrentContext() == nullptr` then proves.
        ///
        /// @param hidden opens the window without showing it, which is what a headless run wants:
        ///        the same renderer with nobody watching, rather than a second path through it.
        void createWindow(const std::filesystem::path& resourceDir, bool hidden);

        /// Sizes the trace, the surface and the viewport to the window once its size has settled.
        ///
        /// **Asked every frame, because a surface cannot be asked whether the window moved.** A
        /// swapchain reports itself out of date when it stops matching the surface it was made for,
        /// and a Wayland surface has no size of its own to be matched against: its `currentExtent`
        /// is `0xFFFFFFFF` by specification, the swapchain's extent being what defines the surface
        /// rather than the other way about. So a present succeeds for ever, and a renderer that
        /// waited to be told would keep the extent the window opened at while the compositor
        /// stretched its picture to whatever the window had become.
        ///
        /// **Acted on only once the window stops moving**, which `sSettleSeconds` says the price of.
        void fitToWindow();

        /// Traces the world the walk has just mirrored, from the eye the frame arrived with.
        ///
        /// **Its refusals are not the frame's.** A world with nothing in it and a camera with no
        /// roll are both reasons not to trace and neither is a reason not to present, so they end
        /// here rather than in `renderFrame` — see the comment on the call.
        ///
        /// `walkMs` and `updateMs` are what the mirror took and what the game took before it,
        /// carried through rather than measured here: the benchmark's row is closed at the end of
        /// the trace and both stretches are over before it starts.
        void traceWorld(const SceneFrame& frame, const Rtx::ExtractionStats& found, double walkMs, double updateMs);

        /// Hands MyGUI's triangles to the renderer, where there is a GUI up at all.
        void drawGui();

        /// Draws whatever asked before there was a world to draw it against.
        void drawDeferredViews();

        /// The interface over whatever was last traced, and the frame onto the screen.
        ///
        /// **Both halves, because every frame this renderer draws ends with both.** A frame with a
        /// world and a frame that is the interface alone reach the screen the same way, so
        /// `renderGui` is one caller of this rather than the place it happens.
        void presentWithGui();

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

        /// Stamps where the frame left this renderer, which is where `Rtx::Timing::Update` starts.
        void leave() { mLeft = std::chrono::steady_clock::now(); }

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

        /// Screenshots, savegame thumbnails and the frozen frame a loading screen puts up.
        ///
        /// The screenshot writer inside it is the same one the OpenGL renderer uses, so the two
        /// write the same file the same way.
        FrameCapture mCapture;

        SDL_Window* mWindow = nullptr;

        /// What the stage was handed. Made here because there is no viewer to make them, and held
        /// because the frame is driven from them.
        osg::ref_ptr<Rtx::PoseUpdate> mUpdateVisitor;

        /// Where `advance` measures reference time from, and the origin the profiler's spans are
        /// stamped against.
        osg::Timer_t mStartTick = 0;

        std::unique_ptr<Rtx::Renderer> mRenderer;

        /// The size the window last reported, and the moment it first reported it. **Not the extent
        /// anything is drawn at** — a surface settles on its own, and `Rtx::FrameExtents` says where
        /// it settled. `sSettleSeconds` is how long the moment has to have been ago.
        ///
        /// **A tick of nought is further back than any tick there is**, which is what makes the
        /// first fit act rather than wait: the constructor fills the extent and leaves this alone.
        std::uint32_t mAskedWidth = 0;
        std::uint32_t mAskedHeight = 0;
        osg::Timer_t mAskedSince = 0;

        /// The engine's scene graph mirrored into what a ray can meet, and the hand-over that
        /// puts it on the device.
        WorldMirror mMirror;

        /// What the last walk found, and what a second walk over the same graph added. Kept because
        /// a report is written at the end of a stop and the walks are over by then.
        Rtx::ExtractionStats mFound;
        Rtx::ExtractionStats mFoundAgain;
        std::uint32_t mUnreadable = 0;

        /// What the CPU stood still for the device, summed over the frames a report came back for
        /// and printed every `sReportEvery` of them.
        ///
        /// **The only instrument on this path.** The harness times a frame by tracing it thirty
        /// times and taking the best; a game cannot, so what it can say is what the last few hundred
        /// frames came to on average — which is the number that matters when the question is whether
        /// this is playable.
        double mSpentMs = 0.0;
        std::uint32_t mTimed = 0;

        /// The run a launcher installed before the engine started, or null for an ordinary
        /// session. `MWRender::Session` says what one is and why it lives here.
        std::unique_ptr<Session> mSession;

        /// The one clock a frame is measured by: how far the simulation steps, how long the trace
        /// is told the frame took, and what OpenMW ages its caches by. `[RTX] fixed step` fills it
        /// once, because it cannot change while a run is being made.
        Rtx::FrameClock mClock;

        /// The knobs a measurement turns, read once at construction. The harness hands them over
        /// in `RendererSpec::mRtx` and a played binary reads `[RTX]`, so the two hosts cannot come
        /// to draw one picture through two differently configured renderers.
        Rtx::RenderProfile mProfile;

        /// When the last frame was handed over, so what `Bench` measures is the whole frame and not
        /// this renderer's slice of it.
        std::chrono::steady_clock::time_point mEntered;
        bool mEnteredOnce = false;

        /// When the frame last left this renderer, so the next one can say what the game spent
        /// between the two. `Rtx::Timing::Update` says what that row answers and why it is timed
        /// here rather than read off a profile.
        ///
        /// **Stamped after every present and after the sweep**, which is every path out of this
        /// renderer — so what it measures is the game's own loop and never this renderer's own
        /// tail. Valid whenever a row is written, because a row is written after a trace and a
        /// trace is made from a call that ends in one of those stamps.
        std::chrono::steady_clock::time_point mLeft;

        /// What the presents since the last row cost, summed.
        ///
        /// **Summed rather than kept, because a span can hold several.** The frame is measured from
        /// one trace to the next, so a present belongs to the span after it — and a loading screen
        /// drives `renderGui` as often as it likes inside one of those. Cleared where the row that
        /// carries it is written.
        double mPresentMs = 0.0;

        /// The frame number the walk and the trace are both stamped with, so what the upscaler
        /// jitters and what the sampler walks are the same sequence the world is counting.
        std::size_t mFrame = 0;

        /// Whether a camera the builder refused has already been reported. `traceWorld` says why
        /// once is the whole of it.
        bool mComplained = false;
    };
}
