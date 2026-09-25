#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <components/sdlutil/vsyncmode.hpp>

#include "debuglines.hpp"
#include "framedigest.hpp"
#include "guirenderer.hpp"
#include "latencyreport.hpp"
#include "memoryreport.hpp"
#include "mesh.hpp"
#include "namedenum.hpp"
#include "pacing.hpp"
#include "reconstruction.hpp"
#include "refusal.hpp"
#include "runs.hpp"
#include "shaders/visibility.h"
#include "slot.hpp"
#include "texturedata.hpp"
#include "upscale.hpp"

struct SDL_Window;

namespace Rtx
{
    class SceneDesc;

    /// Which of the validation layers' checks a run loads. One level and not three switches,
    /// because the three implied one another — either finer check needs the layer under it — and
    /// the two finer checks together took the device down in three runs of four: four of the eight
    /// combinations meant anything, and a fifth was fatal.
    enum class ValidationLevel
    {
        Off,

        /// The core checks.
        On,

        /// The core checks and synchronization validation, which catches a missing barrier. Costs
        /// enough to be opt-in among developers.
        Sync,

        /// The core checks and GPU-assisted validation, which instruments every shader and
        /// catches what a ray query does with its own arguments, at about half the frame rate. The
        /// layer itself asks not to be run beside the core checks, so it is never a default.
        Gpu,
    };

    /// How a level is spelled on a command line and in a report. The one list of the names, for
    /// the reason `sUpscaleNames` gives.
    inline constexpr NamedEnum sValidationNames{ std::array{
        std::pair{ ValidationLevel::Off, std::string_view("off") },
        std::pair{ ValidationLevel::On, std::string_view("on") },
        std::pair{ ValidationLevel::Sync, std::string_view("sync") },
        std::pair{ ValidationLevel::Gpu, std::string_view("gpu") },
    } };

    /// Developer instrumentation. Nobody enables any of this in a run they care about the frame rate
    /// of, and a backend reads whichever of it its API offers.
    struct ValidationOptions
    {
        ValidationLevel mLevel = ValidationLevel::Off;

        /// Stop the process on the first error. Off for a test suite, which provokes errors
        /// deliberately.
        bool mAbortOnError = true;

        /// Whether somebody asked for this by name rather than a build turning it on. A run that
        /// demanded the layers and cannot have them fails naming what is missing, because an empty
        /// log reads as a clean pass; a build that switched them on by default only warns.
        bool mDemanded = false;
    };

    /// Whether the validation layers load without anyone asking: on outside a Release build, off
    /// there because they cost half the frame rate and allocate on the frame path. The build decides
    /// and no setting does, because a setting would put a developer's diagnostic in a player's
    /// configuration file.
#ifdef OPENMW_RTX_VALIDATION_BY_DEFAULT
    inline constexpr bool sValidationByDefault = true;
#else
    inline constexpr bool sValidationByDefault = false;
#endif

    struct RendererOptions
    {
        /// Where the build wrote the compiled shaders for whichever backend this is.
        std::filesystem::path mShaderDirectory;

        /// Where a backend keeps what it compiled, so that a later run need not compile it again.
        /// The user's cache directory (`ConfigurationManager::getCachePath`), because what goes here
        /// is regenerable and worth tens of megabytes. Empty keeps nothing.
        std::filesystem::path mCacheDirectory;

        /// The size the frame is presented at. What it is traced at follows from
        /// `mProfile.mUpscaling.mMode`.
        std::uint32_t mWidth = 1920;
        std::uint32_t mHeight = 1080;

        /// Everything the run decided once about how the picture is made. The upscaling mode is
        /// fixed for the renderer's lifetime bar `Renderer::setUpscale`, and a build that has no
        /// upscaler refuses anything but `Off` at construction.
        RenderProfile mProfile;

        /// Where the frame is shown, or null for a renderer that only reads pixels back. A window
        /// and not a surface, because a surface is a thing an API has. A windowed renderer sizes
        /// itself to the window and ignores `mWidth` and `mHeight`.
        SDL_Window* mWindow = nullptr;

        /// How a present paces the frame, where there is a window. Off for a window somebody
        /// steers by hand and for a measured run, which is why off is the default; the game hands
        /// its own setting over, and `Renderer::setVerticalSync` follows a change to it.
        SDLUtil::VSyncMode mVerticalSync = SDLUtil::VSyncMode::Disabled;

        /// How the driver paces the frame, where there is a window and a driver that paces. Off
        /// for the same windows vertical sync is off for; the game hands its own setting over, and
        /// `Renderer::setPacing` follows a change to it.
        Pacing mPacing;

        ValidationOptions mValidation;

        /// Whether the frame counts for the host: the primary rays that hit anything, and the
        /// values that were not finite at each boundary they crossed — `FrameResult::mHits` and
        /// `mNotFinite`. On by default, so a reader who forgets it gets a number rather than a
        /// silent nought; the game clears it. Not a knob of the run's picture, which is why it is
        /// not in the profile.
        bool mCounting = true;

        /// The video memory the renderer takes its budget to be, in bytes, where the device states
        /// more; nothing to take the device's word. For a run that asks what a smaller card does
        /// with a place: content stops where it would stop there — textures held to a smaller side
        /// first — and what still does not fit is refused as it would be. What the frame itself
        /// holds is never refused, whatever this says.
        std::optional<std::uint64_t> mMemoryBudget;
    };

    /// What a backend holds in one of its slots, as it says so itself. A slot and a scene are one
    /// to one, so nothing here has to name which scene.
    struct SceneHeld
    {
        /// Whether `setScene` has ever filled this slot.
        bool mBuilt = false;

        /// `SceneDesc::getIdentity` of the description the slot was built from: an uploader
        /// handing another description to a slot has to build, because the structures and the
        /// texture array are the first description's, and appending the second's arrivals onto
        /// them would begin past the end of its own table. Nought where nothing was built.
        std::uint64_t mIdentity = 0;

        /// `SceneDesc::getStructureRevision` as it stood at the last `setScene` or `extendScene`.
        std::uint64_t mStructureRevision = 0;

        /// How long the texture table is, which is where an `extendScene`'s arrivals begin — the
        /// length and not the tally, which `SceneStats::mTextureCount` is.
        std::uint32_t mTextureCount = 0;
    };

    /// What a backend reports about the scene it took. The harness's summary line, as a struct.
    struct SceneStats
    {
        InstanceCounts mInstances;

        /// What the renderer holds in acceleration structures and in scene tables: what a video
        /// memory budget is spent against, at the high-water mark, because an allocator gives a
        /// block back only when it empties whole.
        std::uint64_t mStructureBytes = 0;

        /// What the structures occupy inside that; a structure copied tight gives its loose room back.
        std::uint64_t mStructureLiveBytes = 0;

        std::uint64_t mTableBytes = 0;

        /// What the structures answered for and not yet copied tight would come to, or nought where
        /// the device would not say: the difference from `mStructureBytes` is what compacting gives.
        std::uint64_t mCompactableBytes = 0;
        std::uint64_t mCompactableNowBytes = 0;

        /// How many refitted structures the rota has built whole again since the scene was made —
        /// `SceneAcceleration::sRebuildEvery` says the rule.
        std::uint64_t mRebuilt = 0;

        /// Every texture the renderer holds and what those come to, from one walk of the array, so
        /// the two cannot disagree about which slots they counted.
        std::uint32_t mTextureCount = 0;
        std::uint64_t mTextureBytes = 0;

        /// How many of those stand smaller than their files: held to a smaller side where the
        /// device had no room for them as the files are, or past the side it takes.
        std::uint32_t mReducedTextureCount = 0;
    };

    /// What a frame is asked for, beyond where the camera stands.
    struct FrameOptions
    {
        /// How many frames have gone into the running sum, this one included; zero is no averaging.
        /// The sum is kept in floating point, because eight bits would round every sample and clip
        /// the sun's disc.
        std::uint32_t mAccumulate = 0;

        /// How long this frame stands for, in seconds, or nothing to take it off the wall clock. The
        /// eye adapts and the upscaler tunes itself by it, so a measured run states it
        /// (`Misc::FrameClock`) or two runs of one build draw different pictures.
        std::optional<float> mSinceLast = std::nullopt;

        /// What to multiply the measured exposure by: the hour, which the histogram cannot see
        /// (`Rtx::Skylight::mExposureBias`). A fixed `mExposure` is not touched by it.
        float mExposureBias = 1.0f;

        /// What the frame asks of the reconstruction, before the upscaler has its say —
        /// `Reconstruction::resolve` is the rule. A run states it once in its `RenderProfile` and
        /// `forFrame` carries it; a frame of its own may ask otherwise, which is how a reference
        /// and the frame it is compared against come off one renderer.
        ReconstructionRequest mReconstruction;

        /// What to scale the frame by before the display curve, or nothing to measure it off the
        /// frame. One by default, because a measured exposure makes every pixel depend on the whole
        /// frame's histogram; a picture wants it measured, so a run's profile says so.
        std::optional<float> mExposure = 1.0f;

        /// What the game's debug modes drew, over the picture and under the interface. A tool and
        /// not the picture: nothing traces it, and a frame with none pays nothing for it.
        DebugLines mDebug;

        /// Whether the frame leaves its picture in host memory for `FrameResult::mPixels`,
        /// copied by the frame's own commands after the display curve and before the interface,
        /// and digests what it traced for `FrameResult::mDigest` on the way. For a run that hashes
        /// every frame: a copy the frame records rides the queue behind the trace and comes back
        /// with the frame's report, where a readback of the frame just drawn is a submit of its
        /// own and a wait the ring would otherwise overlap.
        bool mReadBack = false;

        /// What a run decided once and what this frame stands for: `accumulate` is the schedule's,
        /// because a warm-up is not averaged in, and `sinceLast` and `exposureBias` are what a
        /// profile cannot know.
        static FrameOptions forFrame(const RenderProfile& profile, const std::uint32_t accumulate,
            const std::optional<float> sinceLast, const float exposureBias)
        {
            return FrameOptions{
                .mAccumulate = accumulate,
                .mSinceLast = sinceLast,
                .mExposureBias = exposureBias,
                .mReconstruction = profile.mReconstruction,
                .mExposure = profile.mExposure,
                .mDebug = {},
            };
        }
    };

    /// One stretch of a frame, measured by the device's own clock — what each dispatch and each
    /// structure build cost, which a wall clock around a submit cannot tell.
    struct GpuSpan
    {
        /// A literal, so the view outlives the span and `GpuBreakdown` may keep it over a run.
        std::string_view mName;
        double mMs = 0.0;
    };

    /// The most zones one frame may open. Sixteen are used; the rest is room to bisect one.
    inline constexpr std::uint32_t sMaxGpuZones = 24;

    /// Where the device spent a frame, in the order the work was recorded, or nothing where it
    /// cannot write timestamps. Owned by the report rather than borrowed from the timer that
    /// measured it: with two frames in flight, the frame that takes this frame's slot begins its
    /// timer before this report is read.
    class GpuZones
    {
    public:
        void clear() { mCount = 0; }

        void add(const GpuSpan& span)
        {
            assert(mCount < sMaxGpuZones && "more zones than a frame may open");
            mSpans[mCount++] = span;
        }

        std::span<const GpuSpan> spans() const { return { mSpans.data(), mCount }; }

    private:
        std::array<GpuSpan, sMaxGpuZones> mSpans{};
        std::uint32_t mCount = 0;
    };

    /// Stores whose value was a NaN or an infinity, at each boundary a frame writes across into
    /// a history or hands to the denoiser — `Shaders::FrameCounts::mNotFinite` by name. Every one
    /// is a logic error: a history that takes one keeps it and spreads it to its neighbours a
    /// frame, so the picture goes black in blocks from there, and nothing on the way refuses it.
    /// Summed over a stop for `Check::Finite`.
    struct NotFinite
    {
        std::uint32_t mFog = 0;
        std::uint32_t mColour = 0;
        std::uint32_t mGuide = 0;

        void add(const NotFinite& more)
        {
            mFog += more.mFog;
            mColour += more.mColour;
            mGuide += more.mGuide;
        }

        std::uint32_t total() const { return mFog + mColour + mGuide; }
    };

    struct FrameResult
    {
        /// Primary rays that hit something: what tells "the cell rendered" from "the camera faced
        /// away" without opening the image. Nought where `RendererOptions::mCounting` was cleared.
        std::uint32_t mHits = 0;

        /// What the frame wrote that was not finite, by boundary. Nought where
        /// `RendererOptions::mCounting` was cleared.
        NotFinite mNotFinite;

        /// What the hold's own clock said the hold came to, in milliseconds: what
        /// `RenderProfile::mStressOverlapMs` asked and the tick past it, on a run that holds, and
        /// nought on one that does not. The loop's reading and not the timer's zone around it,
        /// which takes in the launch and the drain: this is the figure that says the loop did as
        /// it was told, and the zone's excess over it is the card's.
        double mHeldMs = 0.0;

        /// How long the ring waited for this frame; nought where it was already done.
        double mWaitMs = 0.0;

        /// How many frames the ring held when this one was submitted, this one included — one
        /// where the caller waited the frame behind out first, two where it did not. What the
        /// bench's `overlap` figure is taken from, and the number a gate on two in flight asserts.
        std::uint32_t mInFlight = 0;

        GpuZones mGpu;

        /// What put this frame back together, as the renderer resolved it.
        Reconstruction mReconstruction;

        /// Which frame this is, counted by the renderer from its first — the number
        /// `Renderer::getFrameCount` was about to hand the frame when it was drawn, so a caller
        /// that noted something about the frame then can find it now.
        std::uint64_t mFrame = 0;

        /// The picture, where `FrameOptions::mReadBack` asked for it, as `readPixels` lays it
        /// out; empty otherwise. The renderer's own memory: a report collected before a
        /// `renderFrame` reads until the `renderFrame` after that one.
        std::span<const std::uint8_t> mPixels;

        /// What the frame traced, digested, where `FrameOptions::mReadBack` asked; nothing
        /// otherwise.
        std::optional<FrameDigest> mDigest;
    };

    /// One traced image, whichever API produced it: what a scene is handed to, what the interface
    /// is drawn on, what produces a frame, and what a test or a harness reads back. Nothing below
    /// this line is abstracted — buffers, memory, command buffers and pipelines belong to a backend
    /// outright — so a method here is worth a whole scene or a whole frame, and none is reached per
    /// instance or per pixel. `slot` says which scene throughout: the world's, or one
    /// `addViewScene` handed out for a picture inside the interface.
    class Renderer : public GuiRenderer
    {
    public:
        /// Builds everything a scene needs, replacing whatever was there. `textures` are decoded
        /// already and indexed by the scene's texture index, and must outlive the call.
        virtual void setScene(SceneSlot slot, const SceneDesc& scene, std::span<const TextureData> textures) = 0;

        /// The same scene with more in it: geometry and textures appended, nothing renumbered, at
        /// the cost of a cell and never of `setScene`. `arrived` is the textures the scene gained
        /// since the last call, starting at the count this already holds.
        virtual void extendScene(SceneSlot slot, const SceneDesc& scene, std::span<const TextureData> arrived) = 0;

        /// What this slot was last built from, and how far it has been extended since.
        virtual SceneHeld describeHeld(SceneSlot slot) const = 0;

        /// What the last `setScene` or `extendScene` of `slot` could not stand for the device's
        /// sake — a texture past the side it takes, a texture or a mesh it had no room for — for
        /// the scene's owner to report with the rest of what content was refused
        /// (`SceneDesc::refusals`). A texture refused draws the stand-in, and a mesh refused is
        /// left out. Valid until the next call of either for `slot`.
        virtual std::span<const Refusal> getRefusals(SceneSlot slot) const = 0;

        /// Destroys the images of the texture slots a scene gave up. The slots keep their place,
        /// as they do in the scene's own table, and no live material names a freed one.
        virtual void dropTextures(SceneSlot slot, std::span<const Index> textures) = 0;

        /// The same scene with its instances and lights somewhere else and its actors in a new
        /// pose: rebuilds only what says where things are, plus the structure of each mesh
        /// `getDeformed` names. `scene` must be the scene `setScene` was given, because the
        /// placements index into structures this already holds. The world's ripples are read here
        /// too, beside its lights and sprites: what disturbed the water is a per-frame list of the
        /// scene like the other three, and the next `renderFrame` presses what the last placement
        /// or `setScene` read.
        virtual void placeScene(SceneSlot slot, const SceneDesc& scene) = 0;

        /// A scene of its own for a picture inside the interface — the inventory doll, the race
        /// preview — which a ray the frame sends must not find. Slots given back are taken over
        /// before the table grows.
        virtual SceneSlot addViewScene() = 0;

        virtual void dropViewScene(SceneSlot slot) = 0;

        /// The next frame has no usable past: a door, a teleport, a cut. Only the simulation knows,
        /// because a cell load looks like a step from here. Costs one frame of reconstruction.
        virtual void resetHistory() = 0;

        /// Resizes the presented image; what the trace runs at follows from the upscaler, and
        /// `getExtents` says.
        virtual void resize(std::uint32_t width, std::uint32_t height) = 0;

        /// How hard the upscaler works, which decides what the frame is traced at. Rebuilds every
        /// target, so the old extents describe a camera nothing will accept. Throws where the mode
        /// cannot be reached, and a caller that offers the mode catches it and stays where it was.
        virtual void setUpscale(Upscale upscale) = 0;

        /// Which mode the frames are traced under, which a refused mode leaves where it was.
        virtual Upscale getUpscale() const = 0;

        /// How the presented image meets the monitor's refresh. Costs a swapchain rebuild, so a
        /// settings-change call and not a frame one.
        virtual void setVerticalSync(SDLUtil::VSyncMode mode) = 0;

        /// Whether presents are paced by the driver: a window, a driver that paces, and a present
        /// mode the surface paces under. Asked every frame, because a change of present mode
        /// moves the answer; a renderer that answers no is paced by whoever calls it.
        virtual bool pacesFrames() const = 0;

        /// The pacing, changed while the frames run: a menu change, like `setVerticalSync`, and
        /// like it a swapchain rebuild where the present mode moves with it — the mode the driver
        /// paces under is not the one it does not.
        virtual void setPacing(const Pacing& pacing) = 0;

        /// Sleeps until the driver says the next frame may begin, and marks the frame's start and
        /// its input sample. Once per present, before input is read, because what is read after
        /// this is what the frame shows. Nothing where `pacesFrames` is false, and nothing on a
        /// frame still open — the window hidden, a present that failed — because the driver counts
        /// one sleep between two presents.
        virtual void awaitFrame() = 0;

        /// The game's work for the frame is done and the renderer's begins. No frame of the world
        /// is open here: the host closed the last one it placed with `renderFrame` or `skipFrame`.
        ///
        /// @param flash whether this frame carries the analyser's flash: the driver draws a square
        ///        the frame a click landed in, for a latency analyser to time against the click.
        virtual void endSimulation(bool flash) = 0;

        /// The driver's timings of the newest finished frame, or nothing where the driver paces
        /// nothing or has finished nothing yet.
        virtual std::optional<LatencyReport> describeLatency() const = 0;

        /// Traces one frame; `setScene` first, which is an assert. Returns before the device has
        /// drawn it, so the caller can place the next one meanwhile, and `finishFrame` reads back
        /// what it came to. At most two frames are in flight.
        virtual Reconstruction renderFrame(const Shaders::VisibilityConstants& camera, const FrameOptions& options) = 0;

        /// Closes the frame this frame's placements of the world opened, with no trace: where a
        /// placement is not followed by `renderFrame`, because the host refused the camera. Without
        /// it the frame stays open into the host's next, and every placement after takes one more
        /// command buffer. The frame is submitted and numbered and comes back with no report.
        /// Nothing where no placement opened a frame.
        virtual void skipFrame() = 0;

        /// How many frames were closed, by `renderFrame` or by `skipFrame`, which is the number the
        /// next one carries in `FrameResult::mFrame`.
        virtual std::uint64_t getFrameCount() const = 0;

        /// What the oldest unreported frame came to, waiting for it where it is still in flight, or
        /// nothing where every frame drawn was reported. After `renderFrame` that is the frame just
        /// submitted, which a screenshot and a test want.
        virtual std::optional<FrameResult> finishFrame() = 0;

        /// What the oldest unreported frame came to, waiting only where the ring has no room for
        /// the frame about to be placed, or nothing where none has finished. Before `placeScene`,
        /// this is what keeps two frames in flight: the frame behind stays on the device while the
        /// next is placed, and the report is the frame before it. `finishFrame` there instead waits
        /// the frame behind out on every frame, so the device idles from its last pass until the
        /// next placement is submitted — a gap a device-bound frame pays in full.
        virtual std::optional<FrameResult> collectFrame() = 0;

        /// Shows the frame `renderFrame` just produced. A surface that stopped matching the window
        /// is remade by the next `resize`, which the host calls every frame; no answer comes back
        /// here, because the renderer keeps that fact itself. No window is an assert.
        virtual void presentFrame() = 0;

        /// Multi-line report: the device and what it can trace with.
        virtual std::string describeDevice() const = 0;

        /// The knobs the frames are traced under now: what the renderer was made with, and then
        /// whatever a setting moved since. The one copy, so a stop that writes a picture by the
        /// same rules reads the rules the game draws by.
        virtual const RenderProfile& getProfile() const = 0;

        /// Whether instrumentation is actually running, which a missing layer makes different from
        /// having asked. Anything quoting a frame time has to say so.
        virtual bool isValidating() const = 0;

        /// What the renderer has taken from each of the device's memory heaps. Walks every
        /// allocation, so asked once at a place and never once a frame.
        virtual MemoryReport getMemoryReport() const = 0;

        /// What the world scene is made of, as the backend last placed it. Only meaningful once
        /// `Renderer::setScene` has been called for the world.
        virtual const SceneStats& getSceneStats() const = 0;

        /// Copies the traced image into `pixels`, four bytes per pixel, tightly packed. Not on a
        /// frame path: it submits a copy and waits for it, so it is not const.
        virtual void readPixels(std::vector<std::uint8_t>& pixels) = 0;

    protected:
        Renderer() = default;
    };
}
