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

#include "reconstruction.hpp"
#include "reorder.hpp"
#include "shaders/visibility.h"
#include "texturedata.hpp"
#include "upscale.hpp"
#include "wavespectrum.hpp"

struct SDL_Window;

namespace Rtx
{
    /// The world's scene, rather than one a picture inside the interface brought with it.
    ///
    /// Every call that names a scene takes one of these — `sWorld`, or a slot `addViewScene` handed
    /// out — so that the two go through the same code.
    inline constexpr std::uint32_t sWorld = ~0u;

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
        /// deliberately and would otherwise take the whole run down with the first one.
        bool mAbortOnError = true;
    };

    /// Whether the validation layers load without anyone asking.
    ///
    /// **On outside a Release build**, because the alternative is what this fork once spent an
    /// afternoon on: a window that stuttered and froze with nothing in the log, whose cause was a
    /// ray query built with its end behind its start — undefined, silent, and named outright by
    /// GPU-assisted validation the first time it was switched on. A rule the layers can check is one
    /// nobody should have to think to check for.
    ///
    /// They are not free. GPU-assisted validation instruments every shader and costs roughly half
    /// the frame rate, and the layers themselves allocate on the frame path, which is why the test
    /// that counts allocations builds its own device without them. So a Release build never has
    /// them, and `openmw-rtxtool --validation=false` turns them off in any other for a measurement.
    ///
    /// **The build decides this and nothing else does.** It was a `[RTX] validation` setting, which
    /// put a developer's diagnostic in a player's configuration file and let the build that numbers
    /// are quoted from load the layers by leaving a line behind — which is exactly what happened, so
    /// every in-game figure taken under it was measured through them. The build type is the one
    /// thing that already says whether this run is being developed or being measured.
    ///
    /// One answer for both hosts, out of `openmw-rtx`'s own `PUBLIC` definition: the game and the
    /// harness disagreeing about when the layers load is two renderers to debug.
#ifdef OPENMW_RTX_VALIDATION_BY_DEFAULT
    inline constexpr bool sValidationByDefault = true;
#else
    inline constexpr bool sValidationByDefault = false;
#endif

    struct RendererOptions
    {
        /// Where the build wrote the compiled shaders for whichever backend this is.
        std::filesystem::path mShaderDirectory;

        /// The size the frame is **presented** at. What it is traced at follows from `mUpscale`.
        std::uint32_t mWidth = 1920;
        std::uint32_t mHeight = 1080;

        /// Fixed for the renderer's lifetime: an upscaler is brought up once and sized per
        /// resolution, and a build that has none refuses anything but `Off` at construction.
        Upscale mUpscale = Upscale::Off;

        /// Which network the upscaler runs, where one runs at all.
        ///
        /// **Pinned rather than left to the library**, which is what makes two runs comparable: the
        /// default has changed between SDK versions and again between the convolutional and
        /// transformer models, so a frame reconstructed under it is a frame nobody can reproduce.
        Preset mPreset = Preset::D;

        /// Where the frame is shown, or null for a renderer that only reads pixels back.
        ///
        /// **An `SDL_Window*` and not a surface**, because a surface is a thing an API has and SDL
        /// is what both of them are windowed through. The backend asks SDL what its instance needs
        /// and makes the surface itself, so nothing above this line has to know which API it is.
        ///
        /// A windowed renderer sizes itself to the window: `mWidth` and `mHeight` are ignored, and
        /// `resize` is what a resize event calls.
        SDL_Window* mWindow = nullptr;

        ValidationOptions mValidation;

        /// Whether the trace counts the primary rays that hit anything.
        ///
        /// **On by default, and the game is the one place that clears it.** Nothing in the game
        /// reads `FrameResult::mHits`, so its trace is specialized without the atomic — but the
        /// default is the other way round on purpose: a reader who forgets this would get a silent
        /// nought, where a writer who forgets it pays for a number nobody looks at. A wrong figure
        /// is worse than a slow one.
        bool mCountHits = true;

        /// How the trace sorts its threads between the traversal and the shader that resolves what
        /// they found.
        ///
        /// **Off, because off is faster.** The reorder costs this trace 7 to 17 percent at every
        /// view of the default suite and buys nothing back, measured against the same launch with
        /// the sort off. It is kept switchable because the sort is the one half of Stage 2 that can
        /// be turned off: the split into a shader per material kind is what traversal does, and
        /// nothing here decides it.
        Reorder mReorder = Reorder::Off;
    };

    /// One vertex of the GUI: a position already in clip space, a colour packed a byte a channel,
    /// and a texture coordinate.
    ///
    /// **MyGUI's own layout rather than one chosen here.** It fills these itself, by the thousand
    /// every frame the interface is up, and a backend that wanted them any other way would have to
    /// walk every batch and rewrite it. The position arrives in clip space because MyGUI multiplies
    /// widget pixels by the view size for itself.
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

    /// **Trivial and twenty-four bytes**, because a frame of GUI is copied out of the buffer MyGUI
    /// filled rather than walked. Default member initialisers would cost that copy its memcpy.
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

    /// One run of vertices drawn with one texture.
    ///
    /// **A run and not an index range**, because MyGUI hands over triangle lists and no indices: a
    /// batch is a stretch of the vertex buffer and a texture to read while drawing it.
    struct GuiBatch
    {
        /// A slot from `addGuiTexture`.
        std::uint32_t mTexture = 0;
        std::uint32_t mFirstVertex = 0;
        std::uint32_t mVertexCount = 0;
        GuiBlend mBlend = GuiBlend::Over;
    };

    /// What a picture inside the interface is asked for, beyond where its camera stands.
    struct GuiTraceOptions
    {
        /// How much of the texture to fill, from its top-left corner, and what the camera must have
        /// been built for. The rest of the texture is left at `mClear`.
        ///
        /// **Less than the whole of it, routinely.** The inventory doll's window resizes and the
        /// texture behind it does not, so the trace covers a corner of a picture the widget then
        /// shows a corner of.
        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;

        /// What the rest of the texture holds, red first. Transparent black for a picture the GUI
        /// composites over what is behind it, which is every caller there is so far.
        std::array<float, 4> mClear{};

        /// What to trace against: a slot `addViewScene` gave out, or `sWorld` for the one the frame
        /// is drawn from. A map tile is a picture of the world; a doll is not.
        std::uint32_t mScene = sWorld;
    };

    /// What a backend reports about the scene it took. The harness's summary line, as a struct.
    struct SceneStats
    {
        std::uint32_t mInstances = 0;

        /// How many of those traversal has to stop and ask where the holes are — the cost of the
        /// cutout, as a number, so a material change that marks half a cell non-opaque shows up
        /// before a frame time does.
        ///
        /// **Not every instance traversal stops for.** A translucent one is stopped for as well, and
        /// is counted here nowhere: what it costs is a different question, since it never ends the
        /// ray.
        std::uint32_t mCutoutInstances = 0;

        /// How many of those place a mesh whose structure carries an opacity micromap, so traversal
        /// resolves every known microtriangle without stopping and asks only about the unknown
        /// ones. A cutout that is not among them is a mask nothing could bake — an animated one —
        /// or a placement the game is fading, which reads its leaves through the any-hit for as
        /// long as it fades. `Rtx::SceneMicromaps` says which meshes get one.
        std::uint32_t mMicromappedInstances = 0;

        /// What the renderer holds in acceleration structures and in scene tables.
        ///
        /// **What it holds and not what the scene needs**, which is the figure a video memory budget
        /// is spent against: neither allocator ever gives memory back, so both are the high-water
        /// mark of what the run asked for. A block goes to whichever structure asked when no block
        /// had room for it, and a table stays as long as the longest list it ever held.
        ///
        /// **So a route's figures move a little between two runs of one binary, and that is the
        /// answer rather than a fault in it.** What arrives when is the loading threads' to decide,
        /// not the frame clock's, so two runs of a place reach the same content by different orders
        /// and leave the allocators arranged differently. Measured over five runs of `bench` at
        /// Balmora: the instances and every texture figure agree exactly, and
        /// these two land on one of two values 110 KiB and 132 bytes apart — 0.05% and a millionth.
        std::uint64_t mStructureBytes = 0;
        std::uint64_t mTableBytes = 0;

        /// What the opacity micromaps hold, apart from the structures they are attached to. The
        /// figure the level cap is measured against.
        std::uint64_t mMicromapBytes = 0;

        /// Cutout meshes built without a micromap because the slot their mask lives in held no
        /// texture when they were built.
        ///
        /// **A canary, and it should be zero.** An uploader describes every slot a material names
        /// before the structures are built, and a file it cannot read gets a stand-in — so a slot
        /// holding nothing is an arrival that reached the structures ahead of its texture.
        std::uint32_t mMicromapsUntextured = 0;

        /// Every texture the renderer holds, and what those come to.
        ///
        /// **What it holds and not how long its table is.** A slot the scene gave back stays in the
        /// table so that nothing above it is renumbered, and it stands nothing: a hundred of the
        /// shoreline route's six hundred and seventy-two slots are empty by the end of it. Both
        /// figures come from one walk of the array, so they cannot disagree about which slots they
        /// counted. `Renderer::getTextureCount` is the length, and it is a different question.
        ///
        /// **A still is the same on two runs and a route is not, and the terrain's composites are
        /// why.** `Rtx::CompositeQueue` bakes a distant chunk's layer stack on a thread of its own
        /// and a hand-over takes two of the finished ones, so how many have landed when a run ends
        /// is the baker's answer rather than the frame index's. Measured over six runs of the
        /// shoreline route: the same five hundred and thirty content textures every time, and
        /// thirty-four to thirty-six composites at 1.34 MiB apiece. `SceneUploader::setStaged`
        /// waits them all out, which is what makes a `shot` reproducible, and
        /// `SceneUploader::setSettled` is what a run that has to compare itself turns on.
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

    /// A frame's float channels, which are what an upscaler reads and what a test can check.
    enum class Channel
    {
        /// Two floats a pixel: where a surface is now, less where it was, in pixels. Zero where the
        /// ray hit nothing, and zero where the surface stood behind the previous eye — which is not
        /// a place a screen position exists for.
        Motion,

        /// Two floats a pixel: what a rasterizer with this frustum would have written — zero at the
        /// near plane, one at the far one and at every miss — and beside it the distance from the
        /// eye in world units.
        ///
        /// **Two answers because they are two questions.** An upscaler's disocclusion test wants
        /// the clip value it expects of a depth buffer; a filter comparing one surface against
        /// another wants world units, because a tolerance measured against a clip value would mean
        /// something different at every distance — most of that range is spent within a few units
        /// of the eye.
        Depth,

        /// Two floats a pixel: where what the water reflects stood on the previous frame's screen.
        /// Nought everywhere that is not water reflecting a surface.
        ReflectionMotion,

        /// One float a pixel: one where a sprite reached, nought where none did.
        ParticleMask,

        /// One float a pixel: how much of this pixel the reconstruction must not carry forward.
        BiasMask,

        /// Four floats a pixel: the finished frame in linear radiance, at the render extent, before
        /// anything upscales it and before the display curve.
        ///
        /// **What a measurement wants, where `readPixels` is what a picture wants.** Eight bits is
        /// where the filter figures stopped being figures — 0.00253 against a converged reference is
        /// two thirds of a byte at that brightness, so the accumulated frame already sat at the edge
        /// of what a read-back could tell apart, and a tighter number could not be had at all. The
        /// bounce tail is worse off: it is counted in luminance, and the tone curve has spent that
        /// by the time bytes exist.
        ///
        /// Carries whatever the composite wrote, `mAccumulate` included — so a converged reference
        /// and the one frame being measured against it come back through the same call.
        Radiance,

        /// Four floats a pixel: the bounce the trace found, before any filter and before the albedo
        /// is multiplied back in.
        ///
        /// The trace's answer and nothing else's, whatever ran after it.
        Indirect,

        /// Four floats a pixel: the same bounce once the accumulator has averaged it over the
        /// frames this surface has been seen for, and before the cascade blurs it.
        ///
        /// **The channel a firefly is counted in.** The tail `shot --tail` tabulates is a share of
        /// pixels whose bounce luminance passes a threshold, and it is counted here rather than on
        /// `Indirect` so that what the outlier clamp has already been over is what is counted. By
        /// the time bytes exist the albedo has been multiplied in and the display curve has spent
        /// the range that made the number mean something.
        ///
        /// **Only a frame the wavelet denoised has one.** Nothing writes this where the upscaler
        /// denoises for itself, or where no filter ran at all.
        Accumulated,
    };

    /// What a frame is asked for, beyond where the camera stands.
    struct FrameOptions
    {
        /// How many frames have gone into the running sum, this one included. Zero is no averaging.
        ///
        /// **A field here and not of `camera`, because the trace does not read it.** What is being
        /// averaged is the finished picture, which is the last pass's business; a number in the
        /// struct the trace is handed would say it belonged to the trace. The sum is kept in
        /// floating point rather than by averaging the images afterwards: eight bits per channel
        /// would round every sample before adding it, and worse, clip the sun's disc and a water
        /// glint, which are exactly the pixels a filter is most likely to get wrong.
        std::uint32_t mAccumulate = 0;

        /// How long this frame stands for, in seconds, or nothing to take it off the wall clock.
        ///
        /// **The last thing in a frame that ran on the wall.** The eye adapts in real time and an
        /// upscaler tunes itself against how fast a motion vector was travelled, so a game leaves
        /// this empty and the renderer times itself. A measured run cannot: two runs of one build
        /// then adapt by different amounts and draw different pictures, which is a run that cannot
        /// be compared with itself — and everything else a run animates already steps by the frame
        /// index for exactly that reason. A caller with a schedule states it here.
        std::optional<float> mSinceLast = std::nullopt;

        /// What to multiply the exposure this frame measures for itself by. One leaves it alone, and
        /// a fixed `mExposure` is not touched by it at all.
        ///
        /// **The hour, which the histogram cannot see.** `Rtx::Daylight::mExposureBias` is where it
        /// comes from and says why there is one. An interior has no hour and keeps the one here.
        float mExposureBias = 1.0f;

        /// Whether to move the primary ray inside its pixel, by where the frame index falls in a
        /// Halton sequence.
        ///
        /// Overwrites the camera's own `mJitter`, which is otherwise left as the caller wrote it —
        /// zero for anything `makeCamera` made, and an exact offset where something wants one.
        ///
        /// **Off unless something is putting the frames back together.** A jittered frame on its own
        /// is the same picture sampled slightly wrong; it is only worth anything to an upscaler
        /// reconstructing from several, or to a sum that averages them into an antialiased one.
        ///
        /// **Ignored while the renderer is upscaling**, which always jitters: reconstruction from
        /// several frames of the same sample point is reconstruction from one sample.
        bool mJitter = false;

        /// Whether the denoiser runs over the indirect channel.
        ///
        /// **Off is how the answer it is judged against gets made.** A converged reference is the
        /// average of enough unbiased samples, and a filtered sample is not one of those — so a
        /// thousand filtered frames converge on the filter's opinion rather than on the truth.
        ///
        /// **Ignored while the renderer is upscaling**, which denoises for itself: Ray
        /// Reconstruction reconstructs detail from the raw bounce, and handing it a frame already
        /// blurred is asking it to recover what was thrown away.
        bool mFilter = true;

        /// What to scale the frame by before the display curve, or nothing to measure it off the
        /// frame itself.
        ///
        /// **One by default, and that default is what makes a pixel test possible.** A measured
        /// exposure makes every expected value depend on the whole frame's histogram, which is not
        /// a number anybody can hand-compute — and a converged reference wants the exposure held
        /// still across the frames it averages. A picture wants it measured, so the harness turns it
        /// on and the tests leave it alone.
        std::optional<float> mExposure = 1.0f;
    };

    /// What one traced frame came to.
    /// One stretch of a frame, measured by the device's own clock.
    ///
    /// **What a wall clock around a submit cannot tell you.** A frame is a handful of dispatches and
    /// two structure builds, and the CPU sees one number for all of them; these are what each of
    /// them cost, timed by the GPU between the commands that bracket it.
    struct GpuSpan
    {
        /// **A literal, and never a name built for the occasion.** Every zone is opened with one,
        /// so the view outlives the span it arrives in and a report may keep it — which is what
        /// `GpuBreakdown` does over a whole run.
        std::string_view mName;
        double mMs = 0.0;
    };

    struct FrameResult
    {
        /// Primary rays that hit something, which is what tells "the cell rendered" from "the camera
        /// faced away from it" without anyone opening the image.
        /// **Nought where `RendererOptions::mCountHits` was cleared**, which the game does and
        /// nothing else should: the trace is then specialized without the atomic entirely.
        std::uint32_t mHits = 0;

        /// How long `finishFrame` waited for this frame's fence: what the CPU stood still for the
        /// GPU. Nought where the frame was already finished when it was asked for, which is what a
        /// caller with a frame's worth of work to do meanwhile sees.
        double mWaitMs = 0.0;

        /// Where the device spent this frame, in the order the work was recorded — the structure
        /// builds this frame asked for, then the passes that drew it. Empty where the device cannot
        /// write timestamps.
        ///
        /// **Borrowed from the renderer and valid until the frame after next is finished**, because
        /// a frame path that allocated a vector to report its own cost would be measuring itself.
        std::span<const GpuSpan> mGpu;

        /// What put this frame back together, as the renderer resolved it.
        ///
        /// **Reported by the thing that did it.** The alternative was for a caller to work the same
        /// rule out a second time from what it had asked for, which is two copies of a rule that had
        /// already been wrong once by being invisible.
        Reconstruction mReconstruction;
    };

    /// One traced image, whichever API produced it.
    ///
    /// **Nothing below this line is abstracted:** buffers, images, memory, command buffers,
    /// descriptors and pipelines belong to a backend outright and are shared with nothing. An
    /// interface drawn tight enough to hide those would be a mini-Vulkan, and would put a virtual
    /// call inside a frame. So a method here is worth a whole scene or a whole frame, and none is
    /// reached per instance, per light or per pixel.
    ///
    /// A backend that owns a window presents through it. There is no second route off the device
    /// for a frame: `readPixels` copies one back to the host, and nothing exports an allocation.
    class Renderer
    {
    public:
        virtual ~Renderer() = default;

        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        /// Multi-line report: the device and what it can trace with.
        virtual std::string describeDevice() const = 0;

        /// Whether instrumentation is actually running, which is not the same as having asked for
        /// it — a layer can be missing. Anything quoting a frame time has to say so, because a
        /// figure measured under validation is not one to compare against anything.
        virtual bool isValidating() const = 0;

        /// Builds everything a scene needs, replacing whatever was there.
        ///
        /// `textures` are described rather than loaded — `SceneTextures` decodes and the backend
        /// uploads, which is what keeps this library free of a graphics API. They are indexed by the
        /// scene's texture index, so their order is the scene's, and they must outlive the call.
        ///
        /// **`slot` says which scene**: `sWorld` for the one the frame is traced against, or a slot
        /// `addViewScene` handed out for a picture inside the interface. The three calls below take
        /// it too, so a doll gets the same decision a cell does — see `SceneUploader`, which is
        /// where that decision is made once for both.
        virtual void setScene(
            std::uint32_t slot, const SceneDesc& scene, std::span<const TextureData> textures, const SeaState& sea)
            = 0;

        /// The same scene with more in it: geometry and textures appended, nothing renumbered.
        ///
        /// **What a cell arriving costs, and it must not be what `setScene` costs.** Rebuilding
        /// measured at 12 ms for every acceleration structure in the scene and **150 to 225 ms for
        /// the texture array**, because the array was made again from nothing whenever one body
        /// texture appeared. Appending leaves every image where it is.
        ///
        /// `arrived` describes the textures the scene has gained since the last call, in scene
        /// order and starting at the count this already holds — never the whole table, or the
        /// describing and the shading estimate are paid twice for what has not changed.
        ///
        /// Only for a scene whose tables **grew**. A `retain` that closed the gaps renumbers every
        /// index, and the answer to that is still `setScene`.
        virtual void extendScene(
            std::uint32_t slot, const SceneDesc& scene, std::span<const TextureData> arrived, const SeaState& sea)
            = 0;

        /// Say that the next frame has no usable past.
        ///
        /// **A reconstruction accumulates over several frames, and a jump no motion vector can
        /// describe makes every one of them a lie.** Walking through a door, a teleport, a cut: the
        /// camera moves somewhere its previous basis says nothing about, and what is reprojected is
        /// one room onto another.
        ///
        /// **Not derivable from what the renderer sees.** A world scene is built once and then grows
        /// and recycles its slots — `SceneDesc::clear` is never called on it — so `setScene` fires at
        /// startup and not again, and the mirror looks the same across a cell load as it does across
        /// a step. Only the simulation knows, so only the simulation can say.
        ///
        /// Costs one frame of reconstruction, so it is for discontinuities and not for changes.
        virtual void resetHistory() = 0;

        /// How long the renderer's texture table is, which is where `extendScene`'s `arrived` begins.
        ///
        /// **The length and not the tally.** A slot the scene gave back keeps its place so that
        /// nothing above it is renumbered, and it stands nothing — `SceneStats::mTextureCount` is
        /// how many there actually are, and the two differ by every slot a walked-away region left.
        virtual std::uint32_t getTextureCount(std::uint32_t slot) const = 0;

        /// Destroys the images of the texture slots a scene has given up.
        ///
        /// **What stops a region walked away from going on costing its texture memory.** A slot's
        /// image otherwise lives until something takes the slot over, so a route that keeps moving
        /// settles at what it has visited rather than at what is around it.
        ///
        /// The slots keep their place and the array does not shrink, because the scene's own table
        /// does not either: `getTextureCount` still says where an append begins. A backend may leave
        /// the descriptors naming what has gone — no live material names a freed slot, so nothing
        /// indexes one.
        ///
        /// The order against `extendScene`'s arrivals is free: `SceneDesc` keeps the two lists
        /// disjoint, so no slot is ever in both.
        virtual void dropTextures(std::uint32_t slot, std::span<const std::uint32_t> textures) = 0;

        /// The same scene, with its instances and lights somewhere else and its actors in a new pose.
        ///
        /// **What a frame does when the world has moved.** `setScene` rebuilds everything: the
        /// bottom-level acceleration structures, the vertex buffers and the texture array. None of
        /// that changes when a door swings — the geometry is the same geometry — so this rebuilds
        /// only what says where things are, plus the structure of each mesh `getDeformed` names,
        /// which is where a skinned body and a morphed face come in: their triangles are the same
        /// triangles and their vertices are new ones.
        ///
        /// `scene` must be the one `setScene` was given, with `clearPlacement` called and the
        /// instances re-walked: the placements index into structures this already holds.
        virtual void placeScene(std::uint32_t slot, const SceneDesc& scene, const SeaState& sea) = 0;

        /// Only meaningful once `setScene` has been called.
        virtual const SceneStats& getSceneStats() const = 0;

        /// Resizes the **presented** image. What the trace runs at follows from the upscaler, and
        /// `getExtents` is what says. Kept by the backend, so nothing here allocates per frame.
        virtual void resize(std::uint32_t width, std::uint32_t height) = 0;

        /// How the presented image should meet the monitor's refresh.
        ///
        /// **`SDLUtil::VSyncMode` and not a spelling of this fork's own**, because it is the setting
        /// the game already reads and the rasterizer already acts on. A second enum over the same
        /// three values is a second thing to keep in step for nothing.
        ///
        /// Costs a swapchain rebuild where it changes anything, so it is a settings-change call and
        /// not a frame one.
        virtual void setVerticalSync(SDLUtil::VSyncMode mode) = 0;

        /// What the last `resize` settled on. **The camera has to be built for the render extent**,
        /// because the trace's per-pixel ray spread comes from it.
        virtual FrameExtents getExtents() const = 0;

        /// Traces one frame. `setScene` first, which is a contract and so an assert.
        ///
        /// **Returns before the device has drawn it.** The frame is submitted and the call comes
        /// back, so the caller can walk and place the next one while this one is traced; what the
        /// frame came to is read back by `finishFrame`. At most two frames are in flight: the third
        /// waits for the first. What the call decided about reconstruction is returned here because
        /// it is known here.
        virtual Reconstruction renderFrame(const Shaders::VisibilityConstants& camera, const FrameOptions& options) = 0;

        /// What the oldest frame nothing has asked about came to, waiting for it where it is still
        /// in flight, or nothing where every frame drawn has been reported.
        ///
        /// **A frame's report belongs to the frame and not to whichever call did the waiting.** The
        /// ring drains itself when a new frame wants a slot, and what it drained is reported here
        /// like anything else — a caller asking once a frame is answered once a frame.
        ///
        /// **Where the pipeline is paid for and where it pays, and the submit is what divides the
        /// two.** Called after `renderFrame` the oldest frame in flight is the one just submitted,
        /// so the wait is that frame waited out — which is what a screenshot and a pixel test want.
        /// Called before it, the oldest is the frame behind, the walk and the placement have
        /// already run beside the device drawing it, and the fence has usually signalled by the
        /// time the wait is reached. Only the second of those puts two frames in the ring, and a
        /// caller that wants one frame of overlap has to ask for it that way round.
        ///
        /// A caller that never calls it loses nothing but the numbers: a frame's resources are
        /// reclaimed when a later frame needs its slot.
        virtual std::optional<FrameResult> finishFrame() = 0;

        /// Shows the frame `renderFrame` just produced, where this renderer was given a window.
        ///
        /// False means the surface stopped matching the window — a resize, a monitor change, a
        /// compositor restart — and the caller should `resize` and carry on. None of those is an
        /// error, which is why this is not one.
        ///
        /// **A contract and so an assert**: a renderer built without a window has nothing to
        /// present into, and asking it to is a caller's mistake rather than a condition.
        virtual bool presentFrame() = 0;

        /// A texture the GUI draws with, sized once and written whenever it changes.
        ///
        /// **Its own table, separate from the scene's.** That one is indexed by material, sized to
        /// the world and appended to when a cell arrives; a font atlas has nothing to do with either
        /// and outlives every scene the renderer is given.
        ///
        /// Slots a texture gave back are taken over before the table grows.
        virtual std::uint32_t addGuiTexture(std::uint32_t width, std::uint32_t height) = 0;

        /// Which part of a GUI texture a write covers, with the origin at the top left.
        struct GuiRegion
        {
            std::uint32_t mX = 0;
            std::uint32_t mY = 0;
            std::uint32_t mWidth = 0;
            std::uint32_t mHeight = 0;
        };

        /// A rectangle of a texture, four bytes a pixel, tightly packed, row zero first.
        ///
        /// `rgba` is the region's own rows and not slices of a wider image. **The whole surface is
        /// what MyGUI's own interface can say** — it hands out a buffer to fill and takes it back
        /// filled — so most callers pass the whole rectangle; the world map is the one that does
        /// not, and it repaints eighteen pixels square instead of two megabytes.
        ///
        /// For a caller that already holds the pixels. One that is about to produce them wants
        /// `lendGuiTexture` instead, which is this without the copy in front of it.
        virtual void writeGuiTexture(std::uint32_t texture, const GuiRegion& region, std::span<const std::uint8_t> rgba)
            = 0;

        /// Bytes for a rectangle of a texture, to be filled and then handed back with
        /// `sendGuiTexture`. The rows `writeGuiTexture` takes, in the backend's own memory.
        ///
        /// **What MyGUI's `lock` and `unlock` are, said to the backend that has to answer them.** A
        /// backend that lends a buffer of its own instead puts a copy in front of every write, and a
        /// video frame then crosses main memory twice on its way to a device it could have been
        /// written into once.
        ///
        /// The rectangle must lie inside the texture, and only one may be lent at a time. Both are
        /// contracts and so asserts. **Write the span and do not read it back**: a backend may lend
        /// memory the device reads directly, where a read costs far more than the write did.
        virtual std::span<std::uint8_t> lendGuiTexture(std::uint32_t texture, const GuiRegion& region) = 0;

        /// Sends what `lendGuiTexture` handed out. The span stops being writable here.
        virtual void sendGuiTexture(std::uint32_t texture) = 0;

        virtual void dropGuiTexture(std::uint32_t texture) = 0;

        /// Everything the GUI asked to draw, over the finished picture, in one call.
        ///
        /// **After the frame and before it is presented or read.** The GUI's colours are
        /// display-referred — they were picked and drawn against a monitor — so they go on after the
        /// tone curve; putting them through a curve meant for radiance is how a menu comes out grey.
        ///
        /// Vertices are in clip space with +Y up, which is what MyGUI produces; a backend whose own
        /// clip space disagrees answers that for itself.
        virtual void drawGui(std::span<const GuiVertex> vertices, std::span<const GuiBatch> batches) = 0;

        /// Traces the scene from `camera` into a GUI texture rather than into the frame.
        ///
        /// **The pictures inside the interface**: a map tile, the inventory doll, the race preview.
        /// They go straight into the table the GUI draws from, so a picture the interface shows
        /// never comes back to main memory — `readGuiTexture` is there for the one caller that
        /// needs a copy, and pays for it.
        ///
        /// **Not the frame's chain.** Nothing upscales, nothing averages and the exposure is fixed
        /// at one: a doll is a still picture of a subject rather than a frame in a sequence, and
        /// there is no previous one to reconstruct it from. `camera.mTransparentBackground` is what
        /// says the picture stops where nothing was hit.
        virtual void traceGuiTexture(
            std::uint32_t texture, const Shaders::VisibilityConstants& camera, const GuiTraceOptions& options)
            = 0;

        /// A scene of its own for a picture inside the interface to be traced against.
        ///
        /// **Not the world, and not reachable from it.** The inventory doll and the race preview are
        /// groups the game assembled for one picture: nothing in them stands in a cell, they are lit
        /// by a rig of their own, and a ray the frame sends must not be able to find them. Each gets
        /// acceleration structures of its own.
        ///
        /// Slots a scene gave back are taken over before the table grows, as the texture table does.
        virtual std::uint32_t addViewScene() = 0;

        virtual void dropViewScene(std::uint32_t slot) = 0;

        /// The whole of a GUI texture, four bytes a pixel, tightly packed, row zero first.
        ///
        /// **Off the device and so asked for rather than always done.** The global map compositing
        /// what the local map drew is the only caller, and it wants the tile once per cell.
        virtual void readGuiTexture(std::uint32_t texture, std::vector<std::uint8_t>& pixels) = 0;

        /// Copies the traced image into `pixels`, four bytes per pixel, tightly packed.
        /// Not const: it submits a copy and waits for it.
        virtual void readPixels(std::vector<std::uint8_t>& pixels) = 0;

        /// Copies one of the last frame's float channels into `values`, tightly packed.
        ///
        /// The channels an upscaler is handed plus the two a measurement is taken on, and the only
        /// way anything outside the backend can look at any of them. **The frame's, and never a view
        /// scene's**: `traceGuiTexture` draws into targets of its own and leaves these where the
        /// last `renderFrame` left them. Not const: it submits a copy and waits for it.
        virtual void readChannel(Channel channel, std::vector<float>& values) = 0;

        /// Moves whatever the API has complained about since the last call into `errors`.
        ///
        /// **Draining, not peeking**, so that clearing before a test and reading after it are the
        /// same call. Empty where nothing is instrumented, which is the only reason a suite can ask
        /// unconditionally.
        virtual void takeValidationErrors(std::vector<std::string>& errors) = 0;

    protected:
        Renderer() = default;
    };

    /// The SDL window flag a window must be created with for this build's backend to make a
    /// surface on it — `SDL_WINDOW_VULKAN` today, as an SDL flag rather than an enum of our own,
    /// because ORing it into `SDL_CreateWindow` is the only thing anyone does with it.
    ///
    /// **Asked rather than assumed.** A window made with the wrong flag cannot be given a surface
    /// at all. `RendererOptions::mWindow` says nothing above this line has to know which API it is;
    /// without this, opening the window was the one place that did.
    std::uint32_t surfaceWindowFlag();

    /// Builds a renderer, or nothing where this machine cannot run the backend asked for.
    ///
    /// **Null and a reason rather than a throw.** Bring-up failure is the one failure a caller always
    /// wants to act on — a harness skips its GPU tests, the game keeps its rasterizer — and it is the
    /// case that would otherwise oblige this fork to keep exceptions.
    std::unique_ptr<Renderer> createRenderer(const RendererOptions& options, std::string& reason);
}
