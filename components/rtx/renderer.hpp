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
#include "renderprofile.hpp"
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

        /// Whether somebody asked for this by name, rather than a build turning it on.
        ///
        /// A run that asked for the layers and could not have them fails naming what is missing:
        /// without them the log stays empty, and a gate reads that as a clean pass. A build that
        /// switched them on by default only warns.
        bool mDemanded = false;
    };

    /// Whether the validation layers load without anyone asking: on outside a Release build.
    ///
    /// A ray query built with its end behind its start is undefined and silent, and GPU-assisted
    /// validation names it outright. They are not free — GPU-assisted validation costs roughly half
    /// the frame rate and the layers allocate on the frame path — so a Release build never has them,
    /// and `openmw-rtxtool --validation=false` turns them off in any other for a measurement. The
    /// build decides and no setting does, because a setting would put a developer's diagnostic in a
    /// player's configuration file. One `PUBLIC` definition of `openmw-rtx`'s, so the game and the
    /// harness agree.
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

        /// The size the frame is **presented** at. What it is traced at follows from
        /// `mUpscaling.mMode`.
        std::uint32_t mWidth = 1920;
        std::uint32_t mHeight = 1080;

        /// What the upscaler is built with. The mode is fixed for the renderer's lifetime bar
        /// `Renderer::setUpscale`, and a build that has no upscaler refuses anything but `Off` at
        /// construction.
        Upscaling mUpscaling;

        /// Where the frame is shown, or null for a renderer that only reads pixels back.
        ///
        /// An `SDL_Window*` and not a surface, because a surface is a thing an API has: the backend
        /// asks SDL what its instance needs and makes the surface itself. A windowed renderer sizes
        /// itself to the window and ignores `mWidth` and `mHeight`.
        SDL_Window* mWindow = nullptr;

        ValidationOptions mValidation;

        /// Whether the trace counts the primary rays that hit anything.
        ///
        /// On by default, and the game is the one place that clears it: a reader who forgets this
        /// would get a silent nought, where a writer who forgets it pays for a number nobody reads.
        bool mCountHits = true;

        /// Whether the trace counts the see-through surfaces each primary ray crosses. Off by
        /// default: it is a second traversal on every pixel, so a run that left it on would be
        /// timing the census rather than the picture.
        bool mCountCrossings = false;
    };

    /// One vertex of the GUI, in MyGUI's own layout: a position already in clip space (MyGUI
    /// multiplies widget pixels by the view size itself), a colour packed a byte a channel, and a
    /// texture coordinate. MyGUI fills these by the thousand a frame, so a backend that wanted them
    /// any other way would rewrite every batch.
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

    /// Trivial and twenty-four bytes, because a frame of GUI is copied out of the buffer MyGUI
    /// filled rather than walked.
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
        /// been built for. The rest of the texture is left at `mClear`. Routinely less than the
        /// whole: the inventory doll's window resizes and the texture behind it does not.
        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;

        /// What the rest of the texture holds, red first. Transparent black for a picture the GUI
        /// composites over what is behind it, which is every caller there is so far.
        std::array<float, 4> mClear{};

        /// What to trace against: a slot `addViewScene` gave out, or the world's for the one the
        /// frame is drawn from. A map tile is a picture of the world; a doll is not.
        SceneSlot mScene = SceneSlot::world();

        /// Whether to leave a copy of the whole texture where `takeGuiCopy` can hand it to the host.
        /// Asked for rather than always done, because the copy is the one time a picture inside the
        /// interface comes back to main memory.
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

        /// How long the texture table is, which is where an `extendScene`'s arrivals begin. The
        /// length and not the tally: a slot the scene gave back keeps its place and stands nothing,
        /// and `SceneStats::mTextureCount` is how many there actually are.
        std::uint32_t mTextureCount = 0;
    };

    /// What a backend reports about the scene it took. The harness's summary line, as a struct.
    struct SceneStats
    {
        InstanceCounts mInstances;

        /// What the renderer holds in acceleration structures and in scene tables — what it holds
        /// and not what the scene needs, which is the figure a video memory budget is spent against.
        /// An allocator gives a block back only when it empties whole and a table stays as long as
        /// the longest list it ever held, so both sit at the high-water mark, and a route's figures
        /// move a little between two runs because the loading threads decide what arrives when.
        std::uint64_t mStructureBytes = 0;

        /// What the structures occupy inside that. The two part company as soon as anything is
        /// compacted: a structure copied tight gives its loose room back and the room is reused.
        std::uint64_t mStructureLiveBytes = 0;

        std::uint64_t mTableBytes = 0;

        /// What the structures answered for and not yet copied tight would come to, or nought where
        /// there are none and where the device would not say. Beside `mStructureBytes` because the
        /// pair is the question: the difference is what compacting would give back.
        std::uint64_t mCompactableBytes = 0;
        std::uint64_t mCompactableNowBytes = 0;

        /// Every texture the renderer holds, and what those come to — what it holds and not how
        /// long its table is (`SceneHeld::mTextureCount`). Both figures come from one walk of the
        /// array, so they cannot disagree about which slots they counted.
        ///
        /// A still is the same on two runs and a route is not: `Rtx::CompositeQueue` bakes a distant
        /// chunk's layer stack on a thread of its own, so how many have landed when a run ends is
        /// the baker's answer. `CompositeQueue::setSettled` takes the baker's timing out of that
        /// count for a run that has to compare itself.
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

    /// Whether a frame `reconstruction` put back together holds the accumulation to read.
    ///
    /// The accumulator runs only where the wavelet does, so a frame an upscaler denoised and a frame
    /// nothing denoised both leave `FrameImage::Accumulated` unwritten. `readFrameImage` asserts
    /// rather than hand back an image nobody filled, so a caller asks here first.
    inline bool hasFrameImage(const Reconstruction& reconstruction, const FrameImage image)
    {
        if (image != FrameImage::Accumulated)
            return true;

        return reconstruction.filtered();
    }

    /// What a frame is asked for, beyond where the camera stands.
    struct FrameOptions
    {
        /// How many frames have gone into the running sum, this one included. Zero is no averaging.
        ///
        /// Not a field of `camera`, because the trace does not read it: what is averaged is the
        /// finished picture. The sum is kept in floating point rather than by averaging images
        /// afterwards, because eight bits would round every sample and clip the sun's disc.
        std::uint32_t mAccumulate = 0;

        /// How long this frame stands for, in seconds, or nothing to take it off the wall clock.
        ///
        /// The last thing in a frame that reads the wall. The eye adapts in real time and an
        /// upscaler tunes itself against how fast a motion vector was travelled, so a game leaves
        /// this empty. A measured run cannot: two runs of one build would adapt by different amounts
        /// and draw different pictures. `Rtx::FrameClock` states it where a run has a schedule.
        std::optional<float> mSinceLast = std::nullopt;

        /// What to multiply the exposure this frame measures for itself by. One leaves it alone, and
        /// a fixed `mExposure` is not touched by it at all. The hour, which the histogram cannot see:
        /// `Rtx::Skylight::mExposureBias` says why there is one, and an interior keeps the one here.
        float mExposureBias = 1.0f;

        /// What the frame asks of the reconstruction, before the upscaler has its say.
        ///
        /// `mJitter` moves the primary ray inside its pixel by where the frame index falls in a
        /// Halton sequence, overwriting the camera's own `mJitter`. Off unless something is putting
        /// the frames back together: a jittered frame on its own is the same picture sampled
        /// slightly wrong. `mFilter` runs the denoiser over the indirect channel; off is how the
        /// answer it is judged against gets made, because a thousand filtered frames converge on the
        /// filter's opinion rather than on the truth. Both are overruled while the renderer is
        /// upscaling — `Reconstruction::resolve` is the rule and reports what it overruled.
        ReconstructionRequest mReconstruction;

        /// What to scale the frame by before the display curve, or nothing to measure it off the
        /// frame itself.
        ///
        /// One by default, and that default is what makes a pixel test possible: a measured
        /// exposure makes every expected value depend on the whole frame's histogram, and a
        /// converged reference wants the exposure held still across the frames it averages. A
        /// picture wants it measured, so the harness turns it on.
        std::optional<float> mExposure = 1.0f;

        /// What a run decided once, and what this frame stands for. Made and not filled in field by
        /// field, so a field the profile gains reaches a frame here or nowhere.
        ///
        /// @param accumulate how many frames have gone into the running sum, which is the schedule's
        ///        to count rather than the profile's: a warm-up is not averaged in.
        /// @param sinceLast and @param exposureBias are the frame's own — the clock's step and the
        ///        hour's bias — and are the two things a profile cannot know.
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
        /// A literal, and never a name built for the occasion, so the view outlives the span it
        /// arrives in and a report may keep it — which is what `GpuBreakdown` does over a whole run.
        std::string_view mName;
        double mMs = 0.0;
    };

    /// What one traced frame came to.
    struct FrameResult
    {
        /// Primary rays that hit something, which is what tells "the cell rendered" from "the camera
        /// faced away from it" without anyone opening the image. Nought where
        /// `RendererOptions::mCountHits` was cleared.
        std::uint32_t mHits = 0;

        /// See-through surfaces those rays crossed, summed over the frame — the census the peel in
        /// `visibility.rgen` is sized against. Nought unless `RendererOptions::mCountCrossings`
        /// asked for it.
        std::uint32_t mCrossings = 0;

        /// The most any one of those rays crossed, which is what an ordered walk of them would have
        /// to be sized for. A mean over a frame divides a cloud's own depth by how little of the
        /// picture it fills.
        std::uint32_t mCrossingsMost = 0;

        /// How long `finishFrame` waited for this frame's fence: what the CPU stood still for the
        /// GPU. Nought where the frame was already finished when it was asked for.
        double mWaitMs = 0.0;

        /// Where the device spent this frame, in the order the work was recorded. Empty where the
        /// device cannot write timestamps. Borrowed from the renderer and valid until the frame
        /// after next is finished, because a frame path that allocated a vector to report its own
        /// cost would be measuring itself.
        std::span<const GpuSpan> mGpu;

        /// What put this frame back together, as the renderer resolved it — reported by the thing
        /// that did it rather than worked out a second time by a caller.
        Reconstruction mReconstruction;
    };

    /// One traced image, whichever API produced it: what a scene is handed to, what the interface
    /// is drawn on, what produces a frame, and what a test or a harness reads back.
    ///
    /// Nothing below this line is abstracted: buffers, images, memory, command buffers, descriptors
    /// and pipelines belong to a backend outright. An interface tight enough to hide those would be
    /// a mini-Vulkan and would put a virtual call inside a frame, so a method here is worth a whole
    /// scene or a whole frame, and none is reached per instance, per light or per pixel.
    ///
    /// `slot` says which scene throughout: the world's for the one the frame is traced against, or
    /// a slot `addViewScene` handed out for a picture inside the interface. `SceneUploader` is where
    /// the decision between them is made once for both.
    class Renderer
    {
    public:
        virtual ~Renderer() = default;

        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        /// Builds everything a scene needs, replacing whatever was there.
        ///
        /// `textures` are described rather than loaded — `SceneTextures` decodes and the backend
        /// uploads, which is what keeps this library free of a graphics API. They are indexed by the
        /// scene's texture index and must outlive the call.
        virtual void setScene(
            SceneSlot slot, const SceneDesc& scene, std::span<const TextureData> textures, const SeaState& sea)
            = 0;

        /// The same scene with more in it: geometry and textures appended, nothing renumbered.
        ///
        /// What a cell arriving costs, and it must not be what `setScene` costs: a rebuild is every
        /// acceleration structure and the whole texture array made again. `arrived` describes the
        /// textures the scene has gained since the last call, starting at the count this already
        /// holds — never the whole table, or the shading estimate is paid twice for what has not
        /// changed. Only for a scene whose tables grew.
        virtual void extendScene(
            SceneSlot slot, const SceneDesc& scene, std::span<const TextureData> arrived, const SeaState& sea)
            = 0;

        /// What this slot was last built from, and how far it has been extended since.
        virtual SceneHeld describeHeld(SceneSlot slot) const = 0;

        /// Destroys the images of the texture slots a scene has given up, which is what stops a
        /// region walked away from going on costing its texture memory.
        ///
        /// The slots keep their place and the array does not shrink, because the scene's own table
        /// does not either. A backend may leave the descriptors naming what has gone: no live
        /// material names a freed slot. The order against `extendScene`'s arrivals is free, because
        /// `SceneDesc` keeps the two lists disjoint.
        virtual void dropTextures(SceneSlot slot, std::span<const Index> textures) = 0;

        /// The same scene, with its instances and lights somewhere else and its actors in a new pose.
        ///
        /// Rebuilds only what says where things are, plus the structure of each mesh `getDeformed`
        /// names: a skinned body's triangles are the same triangles and its vertices are new ones.
        /// `scene` must be the scene `setScene` was given, with `clearPlacement` called and the
        /// instances re-walked, because the placements index into structures this already holds.
        virtual void placeScene(SceneSlot slot, const SceneDesc& scene, const SeaState& sea) = 0;

        /// A scene of its own for a picture inside the interface to be traced against — the
        /// inventory doll, the race preview. Not the world and not reachable from it: nothing in them
        /// stands in a cell, they are lit by a rig of their own, and a ray the frame sends must not
        /// find them. Slots a scene gave back are taken over before the table grows.
        virtual SceneSlot addViewScene() = 0;

        virtual void dropViewScene(SceneSlot slot) = 0;

        /// What the last `Renderer::resize` settled on. The camera has to be built for the render
        /// extent, because the trace's per-pixel ray spread comes from it.
        virtual FrameExtents getExtents() const = 0;

        /// A texture the GUI draws with, sized once and written whenever it changes. Its own table,
        /// separate from the scene's: a font atlas outlives every scene the renderer is given. Slots
        /// a texture gave back are taken over before the table grows.
        virtual GuiSlot addGuiTexture(std::uint32_t width, std::uint32_t height) = 0;

        /// A rectangle of a texture, four bytes a pixel, tightly packed, row zero first. `rgba` is
        /// the region's own rows and not slices of a wider image.
        ///
        /// For a caller that already holds the pixels. One that is about to produce them wants
        /// `lendGuiTexture` instead, which is this without the copy in front of it.
        virtual void writeGuiTexture(GuiSlot texture, const GuiRegion& region, std::span<const std::uint8_t> rgba) = 0;

        /// Bytes for a rectangle of a texture, to be filled and then handed back with
        /// `sendGuiTexture` — what MyGUI's `lock` and `unlock` are. A backend that lent a buffer of
        /// its own instead would put a copy in front of every write, and a video frame would cross
        /// main memory twice.
        ///
        /// The rectangle must lie inside the texture, and only one may be lent at a time; both are
        /// asserts. Write the span and do not read it back: a backend may lend memory the device
        /// reads directly, where a read costs far more than the write.
        virtual std::span<std::uint8_t> lendGuiTexture(GuiSlot texture, const GuiRegion& region) = 0;

        /// Sends what `lendGuiTexture` handed out. The span stops being writable here.
        virtual void sendGuiTexture(GuiSlot texture) = 0;

        virtual void dropGuiTexture(GuiSlot texture) = 0;

        /// Everything the GUI asked to draw, over the finished picture, in one call.
        ///
        /// After the frame and before it is presented or read: the GUI's colours are
        /// display-referred, so they go on after the tone curve, and through a curve meant for
        /// radiance a menu comes out grey. Vertices are in clip space with +Y up, which is what MyGUI
        /// produces; a backend whose own clip space disagrees answers that for itself.
        virtual void drawGui(std::span<const GuiVertex> vertices, std::span<const GuiBatch> batches) = 0;

        /// Traces the scene from `camera` into a GUI texture rather than into the frame — a map tile,
        /// the inventory doll, the race preview. It goes straight into the table the GUI draws from
        /// and never comes back to main memory unless `GuiTraceOptions::mReadBack` asks for a copy.
        ///
        /// Not the frame's chain: nothing upscales, nothing averages and the exposure is fixed at
        /// one, because a doll is a still and there is no previous frame to reconstruct it from.
        /// `camera.mTransparentBackground` says the picture stops where nothing was hit, and
        /// `camera.mRayMask` is which classes it draws.
        ///
        /// Recorded and not run. The picture rides the next submit the backend makes, so it costs
        /// the frame no wait. It reads the copy of the scene its last placement wrote, and the next
        /// placement of that scene waits for the frame the picture rode.
        virtual void traceGuiTexture(
            GuiSlot texture, const Shaders::VisibilityConstants& camera, const GuiTraceOptions& options)
            = 0;

        /// The copy the last `traceGuiTexture` with `mReadBack` left of `texture`: the whole of it,
        /// four bytes a pixel, tightly packed, row zero first, into `into` as far as it reaches.
        ///
        /// False until the copy has arrived, and never a wait: the frame carrying the trace has to
        /// have been finished, which a frame is two frames on, and the caller asks again next frame.
        virtual bool takeGuiCopy(GuiSlot texture, std::span<std::uint8_t> into) = 0;

        /// Submits every picture recorded and not yet carried, and waits for all of them, so that
        /// every copy asked for can be taken. For the harness and the tests, which want a picture
        /// now and stand outside any frame; a game never calls it.
        virtual void finishGuiTraces() = 0;

        /// The whole of a GUI texture as the device holds it, four bytes a pixel, tightly packed,
        /// row zero first. Submits and waits: for the tests, which read what a draw wrote.
        virtual void readGuiTexture(GuiSlot texture, std::vector<std::uint8_t>& pixels) = 0;

        /// Say that the next frame has no usable past.
        ///
        /// A reconstruction accumulates over several frames, and a jump no motion vector can
        /// describe — a door, a teleport, a cut — makes every one of them a lie. Not derivable from
        /// what the renderer sees: a world scene is built once and then grows and recycles its
        /// slots, so a cell load looks the same as a step. Only the simulation knows. Costs one frame
        /// of reconstruction, so it is for discontinuities and not for changes.
        virtual void resetHistory() = 0;

        /// Resizes the **presented** image. What the trace runs at follows from the upscaler, and
        /// `Renderer::getExtents` is what says. Kept by the backend, so nothing here allocates per frame.
        virtual void resize(std::uint32_t width, std::uint32_t height) = 0;

        /// How hard the upscaler works, which decides what the frame is traced at.
        ///
        /// Rebuilds every target, which is what makes it a setting and not a per-frame option; a
        /// caller holding the old extents is holding a camera nothing will accept. Throws where the
        /// mode cannot be reached — a build with no upscaler, or a driver that cannot run one — which
        /// is the answer to a setting and not a fault, so a caller that offers the mode catches it
        /// and stays where it was.
        virtual void setUpscale(Upscale upscale) = 0;

        /// Which mode the frames are being traced under, which is not always the one that was asked
        /// for: a mode this machine refused leaves the renderer where it was.
        virtual Upscale getUpscale() const = 0;

        /// How the presented image should meet the monitor's refresh. `SDLUtil::VSyncMode` because
        /// it is the setting the game already reads. Costs a swapchain rebuild where it changes
        /// anything, so it is a settings-change call and not a frame one.
        virtual void setVerticalSync(SDLUtil::VSyncMode mode) = 0;

        /// Traces one frame. `setScene` first, which is a contract and so an assert.
        ///
        /// Returns before the device has drawn it, so the caller can walk and place the next one
        /// while this one is traced; what the frame came to is read back by `finishFrame`. At most
        /// two frames are in flight: the third waits for the first.
        virtual Reconstruction renderFrame(const Shaders::VisibilityConstants& camera, const FrameOptions& options) = 0;

        /// What the oldest frame nothing has asked about came to, waiting for it where it is still
        /// in flight, or nothing where every frame drawn has been reported.
        ///
        /// Called after `renderFrame` the oldest frame in flight is the one just submitted, so the
        /// wait is that frame waited out — which is what a screenshot and a pixel test want. Called
        /// before it, the oldest is the frame behind, and the fence has usually signalled by the time
        /// the wait is reached; only that order puts two frames in the ring. A caller that never
        /// calls it loses nothing but the numbers.
        virtual std::optional<FrameResult> finishFrame() = 0;

        /// Shows the frame `renderFrame` just produced, where this renderer was given a window.
        ///
        /// False means the surface stopped matching the window — a resize, a monitor change, a
        /// compositor restart — and the caller should `resize` and carry on. A renderer built without
        /// a window has nothing to present into, which is an assert.
        virtual bool presentFrame() = 0;

        /// Multi-line report: the device and what it can trace with.
        virtual std::string describeDevice() const = 0;

        /// Whether instrumentation is actually running, which is not the same as having asked for
        /// it — a layer can be missing. Anything quoting a frame time has to say so.
        virtual bool isValidating() const = 0;

        /// What the renderer has taken from each of the device's memory heaps. Asked once at a
        /// place and never once a frame: it walks every allocation the backend holds. This counts
        /// what the device gave up, where `SceneStats` counts what the scene is.
        virtual MemoryReport getMemoryReport() const = 0;

        /// What the world scene is made of, as the backend last placed it. Only meaningful once
        /// `Renderer::setScene` has been called for the world.
        virtual const SceneStats& getSceneStats() const = 0;

        /// Copies the traced image into `pixels`, four bytes per pixel, tightly packed.
        /// Not const: it submits a copy and waits for it.
        virtual void readPixels(std::vector<std::uint8_t>& pixels) = 0;

        /// Copies one of the last frame's g-buffer channels into `values`, tightly packed, widened
        /// to floats whatever the channel holds. The frame's, and never a view scene's:
        /// `traceGuiTexture` draws into targets of its own. Not const: it submits a copy and waits.
        virtual void readChannel(Channel channel, std::vector<float>& values) = 0;

        /// The same for one of the two images a frame carries that no channel does: the composite's
        /// own output, and the wavelet's accumulation. `hasFrameImage` first for the accumulation.
        virtual void readFrameImage(FrameImage image, std::vector<float>& values) = 0;

        /// Moves whatever the API has complained about since the last call into `errors`. Draining,
        /// not peeking, so that clearing before a test and reading after it are the same call. Empty
        /// where nothing is instrumented.
        virtual void takeValidationErrors(std::vector<std::string>& errors) = 0;

    protected:
        Renderer() = default;
    };
}
