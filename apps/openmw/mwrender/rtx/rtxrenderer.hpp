#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include <osg/Matrixd>
#include <osg/Node>
#include <osg/Timer>
#include <osg/Vec2f>
#include <osg/ref_ptr>

#include <components/esm3/refnum.hpp>
#include <components/rtx/frameimage.hpp>
#include <components/rtx/frameworld.hpp>
#include <components/rtx/pacing.hpp>
#include <components/rtx/reconstruction.hpp>
#include <components/rtx/shaders/visibility.h>
#include <components/rtx/stepped.hpp>
#include <components/rtxbench/runsetup.hpp>
#include <components/sdlutil/vsyncmode.hpp>
#include <components/settings/categories.hpp>
#include <components/vfs/pathutil.hpp>

#include "../ground.hpp"
#include "../renderer.hpp"
#include "../rendermode.hpp"
#include "debugwalk.hpp"
#include "framereport.hpp"
#include "frametimer.hpp"
#include "rippleemitters.hpp"
#include "rtxrun.hpp"
#include "rtxwindow.hpp"
#include "skyreader.hpp"
#include "viewqueue.hpp"
#include "worldmirror.hpp"

namespace Resource
{
    class ResourceSystem;
}

namespace MyGUI
{
    class ITexture;
}

namespace osg
{
    class Camera;
    class FrameStamp;
    class Image;
    class Stats;
    class Texture2D;
}

namespace osgUtil
{
    class UpdateVisitor;
}

namespace Rtx
{
    class PoseUpdate;
    class Renderer;
}

namespace MyGUIRtx
{
    class RenderManager;
}

namespace MWRender
{
    class TracedView;

    /// The picture as rays find it: a window, a mirror of the scene graph, and a trace. It names a
    /// graphics API in one line — the constructor calls `Rtx::createVulkanRenderer` — and initialises
    /// no OpenGL anywhere: the window is an SDL surface the backend builds on. It drives the frame
    /// itself, the scene-graph half of what `osgViewer::Viewer` does, with no cull because rays go
    /// everywhere; the mirror runs after the update traversal and the present after the mirror.
    class RtxRenderer final : public Renderer
    {
    public:
        /// Throws naming what stopped it — no loader, no device that qualifies, an upscale mode this
        /// build cannot provide — and never falls back to the other renderer. `run` is the
        /// harness's, and outlives this; null is a played session, made from `[RTX]` and the
        /// played answers.
        explicit RtxRenderer(const RendererSpec& spec, const RtxSetup* run = nullptr);
        ~RtxRenderer() override;

        /// No GLSL is compiled here, so no model is given a program: the shader visitor is off, and
        /// a model's state is read as the loader left it.
        void configureResources(Resource::ResourceSystem& resources) override;

        /// A plain group: the lights are gathered on this renderer's own walk, so nothing here
        /// wants a light manager's method.
        osg::ref_ptr<osg::Group> createSceneRoot() override;

        void listAssetsToPreload(
            std::vector<VFS::Path::Normalized>& models, std::vector<VFS::Path::Normalized>& textures) override;

        /// Where the sea stands: `WorldMirror::standSea` says why a cell decides it.
        void addCell(const MWWorld::CellStore* cell) override;

        /// The cell's wading actors stop wading.
        void removeCell(const MWWorld::CellStore* cell) override;

        /// What disturbs the water, as `RippleEmitters` keeps it: an actor that may wade, and a
        /// strike on the surface.
        void addWaterRippleEmitter(const MWWorld::Ptr& ptr) override;
        void removeWaterRippleEmitter(const MWWorld::Ptr& ptr) override;
        void emitWaterRipple(const osg::Vec3f& position) override;

        /// A `TracedGround`: the storage, the worldspace and the active grid, and no chunks.
        std::unique_ptr<Ground> createGround(const GroundSpec& spec) override;

        void detachWorld() override;

        float getGroundReach() const override;
        SDL_Window* getWindow() const override { return mWindow.get(); }

        void attachWorld(RenderingManager& world, osg::Group& worldRoot) override;
        void adoptTraversalRoot(osg::Group& root) override;

        /// Read off the seam at the trace, so nothing to put anywhere.
        void applyViewMask() override {}
        void applyWorldShown() override {}

        /// The driver's sleep where the driver paces, and the seam's limiter where it does not,
        /// asked every frame. What either held is the frame's `Sleep` row.
        bool holdFrame() override;

        /// The limit, as the interval the driver's sleep holds two presents apart.
        void applyFrameRateLimit() override;

        void advance(double simulationTime) override;
        void eventTraversal() override;
        void updateTraversal() override;

        void renderFrame(const SceneFrame& frame) override;

        void notifyCut() override;

        /// A trace into a texture the GUI draws from. A picture of the world traces against the
        /// scene this renderer holds; a subject that stands in no cell is mirrored into a scene of
        /// its own.
        std::unique_ptr<OffscreenView> createWorldView(const OffscreenViewSpec& spec) override;
        std::unique_ptr<SubjectView> createSubjectView(const OffscreenViewSpec& spec) override;

        /// A `TracedOverlay`: the explored cells composited in main memory and mirrored into the
        /// interface.
        std::unique_ptr<MapOverlay> createMapOverlay(const MapOverlaySpec& spec) override;

        /// The frame just presented, read back into a GUI texture. One black texel before anything
        /// has been presented, which is the very first load.
        MyGUI::ITexture& freezeFrame() override;

        /// The interface over whatever was last traced, and the frame onto the screen. Every frame
        /// this renderer draws ends here, with a world in it or not.
        void renderGui() override;

        void capture(osg::Image& image, int width, int height) override;
        void saveScreenshot() override;

        /// A present mode: off is mailbox rather than immediate, and adaptive is relaxed FIFO.
        void setVSync(SDLUtil::VSyncMode mode) override;
        void processChangedSettings(const Settings::CategorySettingVector& changed) override;

        std::unique_ptr<MyGUIPlatform::Platform> createGuiPlatform(
            float scalingFactor, VFS::Path::NormalizedView resourcePath, const std::filesystem::path& logPath) override;

        osg::Timer_t getStartTick() const override { return mStartTick; }

        /*internal:*/
        /// Runs the camera's own update callback and nothing below it: the scene is walked from its
        /// own root, for a node path that must not start at an `ABSOLUTE_RF` camera, and accepting
        /// on the camera afterwards would run every controller twice at one traversal number. A
        /// camera carrying no callback is left alone.
        static void updateEye(osg::Camera& camera, osgUtil::UpdateVisitor& visitor);

        /// Keeps everything a stepped run loads for as long as the run lasts, and leaves a run on
        /// the wall to `[Cells] cache expiry delay`.
        ///
        /// **Which models a run kept was a thread's answer.** An expiry is decided on a loading
        /// thread (`CellPreloader::updateCache` queues it) and keeps what is referenced at the moment
        /// it runs, which the other loading threads change as they go: behind the PBR mods'
        /// textures, two runs of one build dropped different models, read them again as new
        /// objects and laid the scene out two ways from `island-crossing` on. Never expiring is the
        /// schedule's answer, and takes a PBR shot of every view to 7.7 GiB resident at its peak
        /// against 6.8.
        static void setResourceExpiry(Resource::ResourceSystem& resources, const std::optional<float>& step);

        /// The backend the frames and the pictures are traced into, for the harness's own reads.
        Rtx::Renderer& getBackend() { return *mRenderer; }

        /// `WorldMirror::collectStanding`, for the harness's check that no static stands twice.
        void collectStanding(std::vector<ESM::RefNum>& into) const { mMirror.collectStanding(into); }

        /// The pictures inside the interface this renderer holds, for the harness to find the
        /// game's own map tile in.
        ViewQueue& getViews() { return mViews; }

        /// `Rtx::Renderer::getProfile`: the knobs the frames are traced under now — what the
        /// backend was made with, and then whatever a setting moved. The one copy, which the
        /// frame path reads as a stop that writes a picture by the same rules does.
        const Rtx::RenderProfile& getProfile() const { return mRenderer->getProfile(); }

        /// Draws the pictures asked for since the last frame — `ViewQueue::draw` with this
        /// renderer's budget of world views — and answers how long that took. From the frame's
        /// own `Views` phase, and from the run's hook.
        double drawViews();

    private:
        /// Builds everything from the setup, which is spent here. Delegated to, so `mRun` can bind
        /// to `mPlayed` where the host installed none and the setup can be a temporary either way.
        RtxRenderer(const RendererSpec& spec, const RtxSetup* run, const Rtx::RunSetup& setup);

        /// Where a frame stands, asserted at every entry point: the order `renderFrame` takes is
        /// the one order the mirror, the pictures, the backend and the run's hook agree on, and a
        /// call out of its turn — a hook that traced a frame from inside the frame, a picture
        /// drawn under the walk — is a nested frame the backend cannot tell from a frame.
        enum class Phase
        {
            /// Between two frames, which is where the engine's own calls land.
            Between,

            /// The graph is being mirrored.
            Walking,

            /// The scene is being handed to the backend.
            Placing,

            /// The pictures asked for since the last frame are being drawn.
            Views,

            /// The world's own trace is being recorded.
            Tracing,

            /// The run's hook has the frame: it may draw pictures and read the backend, and
            /// nothing else.
            Run,

            /// The interface is being drawn and the frame presented.
            Gui,
        };

        /// How hard the upscaler between the trace and the picture works, which a machine that
        /// cannot reach the mode refuses.
        void setUpscale(Rtx::Upscale upscale);

        /// The Reflex mode the run chose or the menu moved, with the seam's limit as its interval.
        Rtx::Pacing getPacing() const;

        /// Traces the world the walk has just mirrored: the frame behind finished, the scene handed
        /// over, the deferred views drawn, the camera aimed, the frame traced and the report closed.
        /// Its refusals — an empty world, a camera with no roll — are not reasons not to present, so
        /// they end here rather than in `renderFrame`. `since` is how long the frame before this one
        /// stood for, or nothing on the first.
        void traceWorld(
            const SceneFrame& frame, const osg::Matrixd& view, FrameReport& report, std::optional<double> since);

        /// Waits the frame behind out and reads what the device answered for it, into the report.
        void finishBehind(FrameReport& report);

        /// Hands the scene the walk built to the backend, timing the three halves of it into the
        /// report, and says whether it was rebuilt from nothing.
        void handOver(const SceneFrame& frame, FrameReport& report);

        /// Everything the frame is traced with that is the host's to say: the eye the frame
        /// arrived with, built for the render extent, the arms' own, the classes the eye sees, the
        /// sample to take, and the profile's rules for the textures. The world's half is
        /// `Rtx::describeWorld`'s. Nothing for a camera the builder refused, which is reported once.
        std::optional<Rtx::Shaders::VisibilityConstants> describeTrace(
            const SceneFrame& frame, const osg::Matrixd& view);

        /// Traces one frame from `constants`, with the world's sky described into it, and closes
        /// the report with what it came to.
        void trace(const SceneFrame& frame, Rtx::Shaders::VisibilityConstants constants, FrameReport& report,
            std::optional<double> since);

        /// What a measured stop is allowed to look at beyond the report.
        FrameContext describeContext();

        /// Whether the left mouse button went down since the last ask, off SDL's own state.
        bool takeClick();

        /// Hands MyGUI's triangles to the renderer, where there is a GUI up at all.
        void drawGui();

        /// The frame as it stands, as an image `width` by `height` — the frame's own size at nought
        /// — or null before anything was presented. Bottom row first, because every reader here
        /// takes OpenGL's order: `LoadingScreen` inverts V for it, and `osgDB`'s writers expect it.
        osg::ref_ptr<osg::Image> readFrame(int width = 0, int height = 0, Rtx::Channels channels = Rtx::Channels::Rgba);

        Rtx::Stepped<Phase> mPhase{ Phase::Between };

        /// Whether a world is attached: `attachWorld` and `detachWorld` are a pair, and a second
        /// attach would hold the sky's sheets twice and give neither back.
        enum class Attachment
        {
            Detached,
            Attached,
        };

        Rtx::Stepped<Attachment> mAttachment{ Attachment::Detached };

        ViewQueue mViews;

        /// How many pictures of the world one frame draws; the rest wait for the next. Three,
        /// because a cell crossing asks for a row of three map tiles and a fresh load for nine: the
        /// row lands in one frame and the load in three.
        static constexpr std::uint32_t sWorldViewsPerFrame = 3;

        /// MyGUI's backend, which `createGuiPlatform` makes and the window manager owns. Never null
        /// where a frame runs, because the window manager outlives every frame.
        MyGUIRtx::RenderManager* mGui = nullptr;

        /// The frame a loading screen holds up, as the image the GUI mirrors. The texture is made
        /// on the first freeze and the image under it swapped on every one after.
        osg::ref_ptr<osg::Texture2D> mFrozenFrame;

        /// What a frame is read back into, refilled per read and never freed.
        std::vector<std::uint8_t> mReadBack;

        /// The run a played session is, for a host that installed none. Before `mRun`, which binds
        /// to it then.
        PlayedRun mPlayed;

        /// The run this renderer answers to per frame: the harness's, which outlives this, or
        /// `mPlayed`. Bound once and never rebound, so the two hosts cannot come to draw one
        /// picture through two differently answered renderers.
        RtxRun& mRun;

        /// Whether each walk waits for the cell it adopts, where the run says: the one thing
        /// `Rtx::RunSetup` states that outlives the construction it is spent in, because what it
        /// falls back on is the clock's stated step, which the host hands over after. The step
        /// itself is the clock's (`getFrameClock`), and what a frame reads about how the picture is
        /// made is the backend's `getProfile`, which a setting may move and a record made before the
        /// backend may not.
        std::optional<bool> mSettled;

        /// Before the backend, whose surface is on it: the members below die first.
        RtxWindow mWindow;

        /// Made here because there is no viewer to make it, and held because the frame is driven
        /// from it.
        osg::ref_ptr<Rtx::PoseUpdate> mUpdateVisitor;

        /// Where `advance` measures reference time from, and the origin the profiler's spans are
        /// stamped against.
        osg::Timer_t mStartTick = 0;

        std::unique_ptr<Rtx::Renderer> mRenderer;

        /// After the backend, because its slot is in the backend's table and goes back before the
        /// table does.
        std::unique_ptr<MyGUI::ITexture> mFrozenFrameTexture;

        /// The engine's scene graph mirrored into what a ray can meet, and the hand-over that
        /// puts it on the device.
        WorldMirror mMirror;

        /// What the game says about the sky, turned into what the trace is handed. Attached where
        /// the mirror is, because the sheets it holds are the mirror's scene's.
        SkyReader mSky;

        /// What disturbs the water this frame, decided game-side and pressed into the trace's
        /// ripple field.
        RippleEmitters mRipples;

        /// The world root the game hangs its debug nodes on, and the walk that reads them off it
        /// into the frame's lines. Borrowed: the world outlives this, and `detachWorld` lets go.
        osg::Group* mWorldRoot = nullptr;
        DebugWalk mDebugWalk;

        /// What the last walk found, and what a second walk added. Kept because a report is written
        /// at the end of a stop and the walks are over by then.
        WalkReport mWalked;

        /// How far the air has been carried since the run began: the one world fact that is an
        /// integral over the frames rather than a reading of one, so it lives beside the clock.
        Rtx::FogDrift mFogDrift;

        /// Where this frame began and ended inside this renderer, what it presented, and the frame
        /// rate the window's title says: the only instrument on this path, and the number that says
        /// whether this is playable.
        FrameTimer mTimer;

        /// Whether a camera the builder refused has already been reported. `describeTrace` says why
        /// once is the whole of it.
        bool mComplained = false;

        /// How the driver paces the frame, as the run set it and the menu moved it. The interval
        /// beside it is the seam's (`getFrameRateLimit`), so each half has one source.
        Rtx::LatencyMode mLatency;

        /// The left button as `takeClick` last saw it.
        bool mLeftButtonDown = false;
    };
}
