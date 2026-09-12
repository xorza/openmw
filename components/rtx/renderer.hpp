#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <components/sdlutil/vsyncmode.hpp>

#include "frameimage.hpp"
#include "memoryreport.hpp"
#include "mesh.hpp"
#include "reconstruction.hpp"
#include "runs.hpp"
#include "shaders/visibility.h"
#include "slot.hpp"
#include "texturedata.hpp"
#include "upscale.hpp"
#include "wavespectrum.hpp"

struct SDL_Window;

namespace Rtx
{
    class SceneDesc;

    /// Developer instrumentation. Nobody enables any of this in a run they care about the frame rate
    /// of, and a backend reads whichever of it its API offers.
    struct ValidationOptions
    {
        bool mEnabled = false;

        /// Catch missing barriers and wrong stage masks. Costs enough to be opt-in among developers.
        bool mSynchronization = false;

        /// Instrument shaders to catch out-of-bounds access. Costs a great deal.
        bool mGpuAssisted = false;

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
        /// `mUpscaling.mMode`.
        std::uint32_t mWidth = 1920;
        std::uint32_t mHeight = 1080;

        /// What the upscaler is built with. The mode is fixed for the renderer's lifetime bar
        /// `Renderer::setUpscale`, and a build that has no upscaler refuses anything but `Off` at
        /// construction.
        Upscaling mUpscaling;

        /// Where the frame is shown, or null for a renderer that only reads pixels back. A window
        /// and not a surface, because a surface is a thing an API has. A windowed renderer sizes
        /// itself to the window and ignores `mWidth` and `mHeight`.
        SDL_Window* mWindow = nullptr;

        ValidationOptions mValidation;

        /// Whether the trace counts the primary rays that hit anything. On by default, so a reader
        /// who forgets it gets a number rather than a silent nought; the game clears it.
        bool mCountHits = true;

        /// Whether the trace counts the see-through surfaces each primary ray crosses. Off by
        /// default, because it is a second traversal on every pixel.
        bool mCountCrossings = false;
    };

    /// One vertex of the GUI, in MyGUI's own layout: a position already in clip space, a colour
    /// packed a byte a channel, and a texture coordinate. MyGUI fills these by the thousand a frame.
    struct GuiVertex
    {
        float mX;
        float mY;
        float mZ;

        /// Red in the low byte, alpha in the high one — MyGUI's `ColourABGR`.
        std::uint32_t mColour;

        float mU;
        float mV;
    };

    /// A frame of GUI is copied out of the buffer MyGUI filled rather than walked.
    static_assert(sizeof(GuiVertex) == 24, "a GUI vertex is what MyGUI writes, and the buffer is read as its own");
    static_assert(std::is_trivial_v<GuiVertex>);

    /// How a run of GUI reaches what is already on the screen.
    enum class GuiBlend : std::uint32_t
    {
        /// Source alpha over the destination, which is every widget there is.
        Over,

        /// Added to the destination. One layer asks for this — the flash when the player is hit —
        /// and over it the same red reads as a tint on the world rather than light in front of it.
        Additive,
    };

    /// One run of vertices drawn with one texture. A run and not an index range, because MyGUI
    /// hands over triangle lists and no indices.
    struct GuiBatch
    {
        /// A slot from `addGuiTexture`.
        GuiSlot mTexture;
        std::uint32_t mFirstVertex = 0;
        std::uint32_t mVertexCount = 0;
        GuiBlend mBlend = GuiBlend::Over;
    };

    /// Which part of a GUI texture a write covers, with the origin at the top left.
    struct GuiRegion
    {
        std::uint32_t mX = 0;
        std::uint32_t mY = 0;
        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;
    };

    /// What a picture inside the interface is asked for, beyond where its camera stands.
    struct GuiTraceOptions
    {
        /// How much of the texture to fill, from its top-left corner, and what the camera must have
        /// been built for; the rest is left at `mClear`. The inventory doll's window resizes and the
        /// texture behind it does not.
        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;

        /// What the rest of the texture holds, red first: transparent black for a picture the GUI
        /// composites over what is behind it.
        std::array<float, 4> mClear{};

        /// What to trace against: a slot `addViewScene` gave out, or the world's for the one the
        /// frame is drawn from. A map tile is a picture of the world; a doll is not.
        SceneSlot mScene = SceneSlot::world();

        /// Whether to leave a copy of the whole texture where `takeGuiCopy` can hand it to the host,
        /// which is the one time a picture inside the interface comes back to main memory.
        bool mReadBack = false;
    };

    /// What a backend holds in one of its slots, as it says so itself. A slot and a scene are one
    /// to one, so nothing here has to name which scene.
    struct SceneHeld
    {
        /// Whether `setScene` has ever filled this slot.
        bool mBuilt = false;

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

        /// Every texture the renderer holds and what those come to, from one walk of the array, so
        /// the two cannot disagree about which slots they counted.
        std::uint32_t mTextureCount = 0;
        std::uint64_t mTextureBytes = 0;
    };

    /// What the renderer traces at, and what it presents at. Equal wherever nothing is upscaling.
    struct FrameExtents
    {
        /// The trace's own resolution, and so the size of every G-buffer channel, of the camera the
        /// trace is handed, and of what `readChannel` gives back.
        std::uint32_t mRenderWidth = 0;
        std::uint32_t mRenderHeight = 0;

        /// The size of what `readPixels` gives back, which is what `resize` was asked for.
        std::uint32_t mOutputWidth = 0;
        std::uint32_t mOutputHeight = 0;
    };

    /// Whether a frame `reconstruction` put back together holds the accumulation to read: the
    /// accumulator runs only where the wavelet does, and `readFrameImage` asserts rather than hand
    /// back an image nobody filled.
    inline bool hasFrameImage(const Reconstruction& reconstruction, const FrameImage image)
    {
        if (image != FrameImage::Accumulated)
            return true;

        return reconstruction.filtered();
    }

    /// What a frame is asked for, beyond where the camera stands.
    struct FrameOptions
    {
        /// How many frames have gone into the running sum, this one included; zero is no averaging.
        /// The sum is kept in floating point, because eight bits would round every sample and clip
        /// the sun's disc.
        std::uint32_t mAccumulate = 0;

        /// How long this frame stands for, in seconds, or nothing to take it off the wall clock. The
        /// eye adapts and the upscaler tunes itself by it, so a measured run states it
        /// (`Rtx::FrameClock`) or two runs of one build draw different pictures.
        std::optional<float> mSinceLast = std::nullopt;

        /// What to multiply the measured exposure by: the hour, which the histogram cannot see
        /// (`Rtx::Skylight::mExposureBias`). A fixed `mExposure` is not touched by it.
        float mExposureBias = 1.0f;

        /// What the frame asks of the reconstruction, before the upscaler has its say —
        /// `Reconstruction::resolve` is the rule. Jitter is off unless something puts the frames
        /// back together; the filter is off for a reference, because a thousand filtered frames
        /// converge on the filter's opinion.
        ReconstructionRequest mReconstruction;

        /// What to scale the frame by before the display curve, or nothing to measure it off the
        /// frame. One by default, because a measured exposure makes every pixel depend on the whole
        /// frame's histogram; a picture wants it measured, so the harness turns it on.
        std::optional<float> mExposure = 1.0f;

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

    struct FrameResult
    {
        /// Primary rays that hit something: what tells "the cell rendered" from "the camera faced
        /// away" without opening the image. Nought where `RendererOptions::mCountHits` was cleared.
        std::uint32_t mHits = 0;

        /// See-through surfaces those rays crossed, summed over the frame, and the most any one ray
        /// crossed — what the peel in `visibility.rgen` is sized against. Nought unless
        /// `RendererOptions::mCountCrossings` asked.
        std::uint32_t mCrossings = 0;
        std::uint32_t mCrossingsMost = 0;

        /// How long `finishFrame` waited for this frame's fence; nought where it was already done.
        double mWaitMs = 0.0;

        /// Where the device spent this frame, in the order the work was recorded, or empty where it
        /// cannot write timestamps. Borrowed from the renderer and valid until the frame after next
        /// is finished, so that reporting a frame's cost allocates nothing.
        std::span<const GpuSpan> mGpu;

        /// What put this frame back together, as the renderer resolved it.
        Reconstruction mReconstruction;
    };

    /// One traced image, whichever API produced it: what a scene is handed to, what the interface
    /// is drawn on, what produces a frame, and what a test or a harness reads back. Nothing below
    /// this line is abstracted — buffers, memory, command buffers and pipelines belong to a backend
    /// outright — so a method here is worth a whole scene or a whole frame, and none is reached per
    /// instance or per pixel. `slot` says which scene throughout: the world's, or one
    /// `addViewScene` handed out for a picture inside the interface.
    class Renderer
    {
    public:
        virtual ~Renderer() = default;

        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        /// Builds everything a scene needs, replacing whatever was there. `textures` are decoded
        /// already and indexed by the scene's texture index, and must outlive the call.
        virtual void setScene(SceneSlot slot, const SceneDesc& scene, std::span<const TextureData> textures) = 0;

        /// The same scene with more in it: geometry and textures appended, nothing renumbered, at
        /// the cost of a cell and never of `setScene`. `arrived` is the textures the scene gained
        /// since the last call, starting at the count this already holds.
        virtual void extendScene(SceneSlot slot, const SceneDesc& scene, std::span<const TextureData> arrived) = 0;

        /// What this slot was last built from, and how far it has been extended since.
        virtual SceneHeld describeHeld(SceneSlot slot) const = 0;

        /// Destroys the images of the texture slots a scene gave up. The slots keep their place,
        /// as they do in the scene's own table, and no live material names a freed one.
        virtual void dropTextures(SceneSlot slot, std::span<const Index> textures) = 0;

        /// The same scene with its instances and lights somewhere else and its actors in a new
        /// pose: rebuilds only what says where things are, plus the structure of each mesh
        /// `getDeformed` names. `scene` must be the scene `setScene` was given, because the
        /// placements index into structures this already holds.
        virtual void placeScene(SceneSlot slot, const SceneDesc& scene) = 0;

        /// A scene of its own for a picture inside the interface — the inventory doll, the race
        /// preview — which a ray the frame sends must not find. Slots given back are taken over
        /// before the table grows.
        virtual SceneSlot addViewScene() = 0;

        virtual void dropViewScene(SceneSlot slot) = 0;

        /// What the last `Renderer::resize` settled on. The camera has to be built for the render
        /// extent, because the trace's per-pixel ray spread comes from it.
        virtual FrameExtents getExtents() const = 0;

        /// A texture the GUI draws with, sized once and written whenever it changes, in a table of
        /// its own because a font atlas outlives every scene.
        virtual GuiSlot addGuiTexture(std::uint32_t width, std::uint32_t height) = 0;

        /// Bytes for a rectangle of a texture, four a pixel, tightly packed, row zero first, to be
        /// filled and handed back with `sendGuiTexture`: MyGUI's `lock` and `unlock`. The rectangle
        /// must lie inside the texture and only one may be lent at a time, both asserts. Write the
        /// span and do not read it back: a backend may lend memory the device reads directly.
        virtual std::span<std::uint8_t> lendGuiTexture(GuiSlot texture, const GuiRegion& region) = 0;

        /// Sends what `lendGuiTexture` handed out. The span stops being writable here.
        virtual void sendGuiTexture(GuiSlot texture) = 0;

        virtual void dropGuiTexture(GuiSlot texture) = 0;

        /// Everything the GUI asked to draw, over the finished picture, after the tone curve because
        /// the GUI's colours are display-referred. Vertices are in clip space with +Y up, as MyGUI
        /// produces them.
        virtual void drawGui(std::span<const GuiVertex> vertices, std::span<const GuiBatch> batches) = 0;

        /// Traces the scene from `camera` into a GUI texture rather than into the frame: a map
        /// tile, the inventory doll. Not the frame's chain — nothing upscales or averages and the
        /// exposure is one, because a still has no previous frame. Recorded and not run: the picture
        /// rides the next submit, reads the copy of the scene its last placement wrote, and the next
        /// placement of that scene waits for the frame it rode.
        virtual void traceGuiTexture(
            GuiSlot texture, const Shaders::VisibilityConstants& camera, const GuiTraceOptions& options)
            = 0;

        /// The copy the last `traceGuiTexture` with `mReadBack` left of `texture`, four bytes a
        /// pixel, tightly packed, row zero first, into `into` as far as it reaches. False until the
        /// copy arrived, which is two frames on, and never a wait.
        virtual bool takeGuiCopy(GuiSlot texture, std::span<std::uint8_t> into) = 0;

        /// Submits every picture recorded and not yet carried and waits for them, for a harness or a
        /// test standing outside any frame. A game never calls it.
        virtual void finishGuiTraces() = 0;

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

        /// The sea every scene is traced with, `SeaState{}` until told. A settings-change call: it
        /// uploads a spectrum and waits the frames in flight out first.
        virtual void setSea(const SeaState& sea) = 0;

        /// How the presented image meets the monitor's refresh. Costs a swapchain rebuild, so a
        /// settings-change call and not a frame one.
        virtual void setVerticalSync(SDLUtil::VSyncMode mode) = 0;

        /// Traces one frame; `setScene` first, which is an assert. Returns before the device has
        /// drawn it, so the caller can place the next one meanwhile, and `finishFrame` reads back
        /// what it came to. At most two frames are in flight.
        virtual Reconstruction renderFrame(const Shaders::VisibilityConstants& camera, const FrameOptions& options) = 0;

        /// What the oldest unreported frame came to, waiting for it where it is still in flight, or
        /// nothing where every frame drawn was reported. After `renderFrame` that is the frame just
        /// submitted, which a screenshot wants; before it, the frame behind, whose fence has usually
        /// signalled, which is the only order that keeps two frames in the ring.
        virtual std::optional<FrameResult> finishFrame() = 0;

        /// Shows the frame `renderFrame` just produced. False means the surface stopped matching
        /// the window and the caller should `resize` and carry on. No window is an assert.
        virtual bool presentFrame() = 0;

        /// Multi-line report: the device and what it can trace with.
        virtual std::string describeDevice() const = 0;

        /// Whether instrumentation is actually running, which a missing layer makes different from
        /// having asked. Anything quoting a frame time has to say so.
        virtual bool isValidating() const = 0;

        /// What the renderer has taken from each of the device's memory heaps. Walks every
        /// allocation, so asked once at a place and never once a frame.
        virtual MemoryReport getMemoryReport() const = 0;

        /// What the world scene is made of, as the backend last placed it. Only meaningful once
        /// `Renderer::setScene` has been called for the world.
        virtual const SceneStats& getSceneStats() const = 0;

        /// What reads a frame back. None of the five below is on a frame path: each submits a copy
        /// and waits for it, so none is const.

        /// Copies the traced image into `pixels`, four bytes per pixel, tightly packed.
        virtual void readPixels(std::vector<std::uint8_t>& pixels) = 0;

        /// Copies one of the last frame's g-buffer channels into `values`, tightly packed, widened
        /// to floats whatever the channel holds. The frame's, never a view scene's.
        virtual void readChannel(Channel channel, std::vector<float>& values) = 0;

        /// The same for one of the two images a frame carries that no channel does: the composite's
        /// own output, and the wavelet's accumulation. `hasFrameImage` first for the accumulation.
        virtual void readFrameImage(FrameImage image, std::vector<float>& values) = 0;

        /// The whole of a GUI texture as the device holds it, four bytes a pixel, tightly packed,
        /// row zero first.
        virtual void readGuiTexture(GuiSlot texture, std::vector<std::uint8_t>& pixels) = 0;

        /// Moves whatever the API has complained about since the last call into `errors`, so that
        /// clearing before a test and reading after it are the same call.
        virtual void takeValidationErrors(std::vector<std::string>& errors) = 0;

    protected:
        Renderer() = default;
    };
}
