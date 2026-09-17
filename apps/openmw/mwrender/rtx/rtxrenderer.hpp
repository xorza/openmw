#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <SDL_video.h>
#include <osg/Node>
#include <osg/Timer>
#include <osg/ref_ptr>

#include <components/esm3/refnum.hpp>
#include <components/rtx/frameclock.hpp>
#include <components/rtx/frameimage.hpp>
#include <components/rtx/frameworld.hpp>
#include <components/rtx/reconstruction.hpp>
#include <components/rtx/shaders/visibility.h>
#include <components/rtx/stepped.hpp>
#include <components/rtxbench/frametimes.hpp>
#include <components/sdlutil/vsyncmode.hpp>
#include <components/settings/categories.hpp>
#include <components/vfs/pathutil.hpp>

#include "../ground.hpp"
#include "../renderer.hpp"
#include "../rendermode.hpp"
#include "debugwalk.hpp"
#include "framereport.hpp"
#include "rippleemitters.hpp"
#include "rtxrun.hpp"
#include "skyreader.hpp"
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
    struct PoseMoment;

    /// A camera's cull mask as the trace reads it: which `Rtx::InstanceClass`es its rays meet, and
    /// whether it draws the sprites. The one translation, so both renderers read one mask.
    std::uint32_t rayMaskOf(osg::Node::NodeMask cullMask);

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
        Ground createGround(const GroundSpec& spec) override;

        void enableReference(ESM::RefNum refnum, bool enabled) override;
        void forgetReferences() override;
        void detachWorld() override;

        float getGroundReach() const override;
        SDL_Window* getWindow() const override { return mWindow.get(); }

        void attachWorld(RenderingManager& world, osg::Group& worldRoot) override;
        void adoptTraversalRoot(osg::Group& root) override;

        /// Read off the seam at the trace, so nothing to put anywhere.
        void applyViewMask() override {}
        void applyWorldShown() override {}

        void tickSchedule() override;
        double beginFrame(double measured) override;
        void advance(double simulationTime) override;
        void eventTraversal() override;
        void updateTraversal() override;

        void renderFrame(const SceneFrame& frame) override;

        void notifyWorldSpaceChanged() override;

        /// A trace into a texture the GUI draws from. A picture of the world traces against the
        /// scene this renderer holds; a subject that stands in no cell is mirrored into a scene of
        /// its own.
        std::unique_ptr<OffscreenView> createWorldView(const OffscreenViewSpec& spec) override;
        std::unique_ptr<SubjectView> createSubjectView(const OffscreenViewSpec& spec) override;

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

        /// The backend a view traces into. This and the four below are what a traced view, drawn
        /// on a later frame than the one that asked, needs back from the renderer that made it.
        Rtx::Renderer& getBackend() { return *mRenderer; }

        /// `WorldMirror::collectStanding`, for the harness's check that no static stands twice.
        void collectStanding(std::vector<ESM::RefNum>& into) const { mMirror.collectStanding(into); }

        /// `Rtx::Renderer::getProfile`: the knobs the frames are traced under now, for a stop that
        /// writes a picture by the same rules. Not `mSetup`'s, which is what the backend was made
        /// with and stays so.
        const Rtx::RenderProfile& getProfile() const { return mRenderer->getProfile(); }

        /// The moment a subject is posed at, for a view drawn inside this frame's window.
        PoseMoment describePose();

        /// Draws `view` in the next frame's window, after the world's placement and before its
        /// trace, where the copy of the tables a picture reads is the frame's own; drawn where asked,
        /// a picture made a wait of every placement that could still follow it. Asked twice in one
        /// frame is drawn once.
        void redraw(TracedView& view);

        /// Draws the pictures asked for since the last frame, every subject's and up to
        /// `sWorldViewsPerFrame` of the world's, and answers how long that took.
        double drawViews();

        /// Takes a view off that list, because it is going away.
        void forgetView(TracedView& view);

    private:
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

        /// Where one frame began and ended inside this renderer, and what it presented. Stamps and not
        /// a report: `Rtx::Timing::Update` is the gap between one frame leaving this renderer and the
        /// next arriving, and only the object that stamps both ends can measure it.
        class FrameSpan
        {
        public:
            /// Opens a frame. @return how long since the last frame opened, in milliseconds, and nothing
            /// on the first. Every frame `renderFrame` is handed opens one, traced or not.
            std::optional<double> enter(std::chrono::steady_clock::time_point now);

            /// Stamps where the frame left this renderer. Every path out calls it, so what the next frame
            /// measures is the game's own loop and never this renderer's tail.
            void leave(std::chrono::steady_clock::time_point now) { mLeft = now; }

            /// How long the game spent between the last `leave` and `now`.
            double sinceLeft(std::chrono::steady_clock::time_point now) const;

            /// Adds what one present cost. Summed, because a loading screen presents as often as it
            /// likes inside one span.
            void addPresent(double ms) { mPresentMs += ms; }

            /// What the presents since the last frame came to, and starts the sum again.
            double takePresent();

        private:
            std::optional<std::chrono::steady_clock::time_point> mEntered;
            std::chrono::steady_clock::time_point mLeft;
            double mPresentMs = 0.0;
        };

        /// What this renderer says about its own speed: the window's title once a second, because this
        /// renderer has no overlay, and the wait it averaged over the last few hundred frames.
        class SpeedReport
        {
        public:
            /// Adds one frame's whole time. @return the title to set, or empty until a second has run
            /// out. Never allocates: the text is written into this object's own bytes.
            std::string_view addFrame(double frameMs);

            /// Adds what the CPU stood still for the device on one frame. Counted only where a frame
            /// answered, or the average divides by frames that contributed nothing. @return whether a
            /// line is due, which starts the sum again.
            bool addWait(double waitMs);

            /// What the frames of the line just due came to. Read after `addWait` answers true.
            double getWaitMs() const { return mReportedMs / static_cast<double>(mReported); }
            std::uint32_t getFrames() const { return mReported; }

        private:
            /// How many answered frames one line covers.
            static constexpr std::uint32_t sReportEvery = 300;

            Rtx::FrameRate mRate;

            /// What the window's title is written from, once a second and never allocated.
            std::array<char, 96> mTitle{};

            /// The sum and the count of the line being gathered.
            double mSpentMs = 0.0;
            std::uint32_t mTimed = 0;

            /// What the line that has just come due covers, held so the caller may read it after the
            /// sum has started again. Both halves, or a sum left running reports every line so far
            /// over one line's frames — a wait that read as climbing a tenth of a millisecond a
            /// line for the life of the session.
            double mReportedMs = 0.0;
            std::uint32_t mReported = 0;
        };

        /// Makes the SDL window the backend builds its surface on; `hidden` is a headless run, the
        /// same renderer with nobody watching.
        void createWindow(bool hidden);

        /// How hard the upscaler between the trace and the picture works, as `RTX / upscale`
        /// names it.
        void setUpscale(std::string_view name);

        /// Sizes the trace, the surface and the viewport to the window once its size has settled.
        /// Asked every frame, because a Wayland surface has no size of its own — its `currentExtent`
        /// is `0xFFFFFFFF` by specification — so a present succeeds for ever and the compositor
        /// stretches the picture to whatever the window became. `sSettleSeconds` is the wait.
        void fitToWindow();

        /// Traces the world the walk has just mirrored: the frame behind finished, the scene handed
        /// over, the deferred views drawn, the camera aimed, the frame traced and the report closed.
        /// Its refusals — an empty world, a camera with no roll — are not reasons not to present, so
        /// they end here rather than in `renderFrame`. `since` is how long the frame before this one
        /// stood for, or nothing on the first.
        void traceWorld(const SceneFrame& frame, FrameReport& report, std::optional<double> since);

        /// Waits the frame behind out and reads what the device answered for it, into the report.
        void finishBehind(FrameReport& report);

        /// Hands the scene the walk built to the backend, timing the three halves of it into the
        /// report, and says whether it was rebuilt from nothing.
        void handOver(const SceneFrame& frame, FrameReport& report);

        /// Everything the frame is traced with that is the host's to say: the eye the frame
        /// arrived with, built for the render extent, the arms' own, the classes the eye sees, the
        /// sample to take, and the profile's rules for the textures. The world's half is
        /// `Rtx::describeWorld`'s. Nothing for a camera the builder refused, which is reported once.
        std::optional<Rtx::Shaders::VisibilityConstants> describeTrace(const SceneFrame& frame);

        /// Traces one frame from `constants`, with the world's sky described into it, and closes
        /// the report with what it came to.
        void trace(const SceneFrame& frame, Rtx::Shaders::VisibilityConstants constants, FrameReport& report,
            std::optional<double> since);

        /// What a measured stop is allowed to look at beyond the report.
        FrameContext describeContext();

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

        /// Whether the world has been handed to the backend at least once.
        bool mHasScene = false;

        /// Pictures asked for and not yet drawn, in the order asked. Raw pointers because the
        /// caller owns every view, and `forgetView` keeps that sound.
        std::vector<TracedView*> mDeferred;

        /// The list a flush walks, swapped out of `mDeferred` so a redraw cannot grow what is being
        /// iterated. Kept, because this sits on the frame path.
        std::vector<TracedView*> mDrawing;

        /// How many pictures of the world one frame draws; the rest wait for the next. A fresh load
        /// asks for nine map tiles at once and a cell crossing for a row of three. A picture of a
        /// subject is never held back.
        static constexpr std::uint32_t sWorldViewsPerFrame = 3;

        /// MyGUI's backend, which `createGuiPlatform` makes and the window manager owns. Never null
        /// where a frame runs, because the window manager outlives every frame.
        MyGUIRtx::RenderManager* mGui = nullptr;

        /// The frame a loading screen holds up, as the image the GUI mirrors. The texture is made
        /// on the first freeze and the image under it swapped on every one after.
        osg::ref_ptr<osg::Texture2D> mFrozenFrame;

        /// What a frame is read back into, refilled per read and never freed.
        std::vector<std::uint8_t> mReadBack;

        /// Before the backend, whose surface is on it: the members below die first.
        std::unique_ptr<SDL_Window, void (*)(SDL_Window*)> mWindow{ nullptr, SDL_DestroyWindow };

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

        /// The size the window last reported and the moment it first reported it — not the extent
        /// anything is drawn at, which `Rtx::FrameExtents` says. A tick of nought is further back
        /// than any tick, which is what makes the first fit act rather than wait.
        std::uint32_t mAskedWidth = 0;
        std::uint32_t mAskedHeight = 0;
        osg::Timer_t mAskedSince = 0;

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
        std::uint32_t mUnreadable = 0;

        /// What the CPU stood still for the device, and the frame rate the window's title says: the
        /// only instrument on this path, and the number that says whether this is playable.
        SpeedReport mSpeed;

        /// What the run was made with, and the run itself: the setup the harness installed before
        /// the engine started, or the played session's own, made from `[RTX]` and the played
        /// answers. The two hosts cannot come to draw one picture through two differently configured
        /// renderers, because both reach the renderer through this one record. The run inside it is
        /// borrowed: `RtxSetup::mRun` says whose it is and that it outlives this.
        const RtxSetup mInstalled;

        /// How far the air has been carried since the run began: the one world fact that is an
        /// integral over the frames rather than a reading of one, so it lives beside the clock.
        Rtx::FogDrift mFogDrift;

        /// The one clock a frame is measured by: how far the simulation steps, how long the trace
        /// is told the frame took, and what OpenMW ages its caches by. A run's step fills it once,
        /// because it cannot change while a run is being made; a played session follows the wall.
        Rtx::FrameClock mClock;

        /// Where this frame began and ended inside this renderer, and what it presented.
        FrameSpan mSpan;

        /// The frame number the walk and the trace are both stamped with, so what the upscaler
        /// jitters and what the sampler walks are the same sequence the world is counting.
        std::size_t mFrame = 0;

        /// Whether a camera the builder refused has already been reported. `describeTrace` says why
        /// once is the whole of it.
        bool mComplained = false;
    };
}
