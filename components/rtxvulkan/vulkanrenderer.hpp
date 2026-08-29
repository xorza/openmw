#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <components/rtx/instancerecord.hpp>
#include <components/rtx/renderer.hpp>

#include "accumulatepass.hpp"
#include "atrouspass.hpp"
#include "bloompass.hpp"
#include "buffer.hpp"
#include "commands.hpp"
#include "compositepass.hpp"
#include "device.hpp"
#include "exposurepass.hpp"
#include "fogtile.hpp"
#include "frameslots.hpp"
#include "gbuffer.hpp"
#include "gputimer.hpp"
#include "graveyard.hpp"
#include "guipass.hpp"
#include "guitextures.hpp"
#include "instance.hpp"
#include "tonepass.hpp"
#include "wavepass.hpp"

namespace Rtx
{
#ifdef OPENMW_RTX_DLSS
    class Dlss;
    class DlssPass;
#endif
    class GBuffer;
    class Image;

    class Presenter;
    class SceneAcceleration;
    class SceneBuffers;
    class TextureArray;
    class VisibilityPass;

    /// `Renderer` over Vulkan.
    ///
    /// This is the shot command and the pixel tests' fixture merged: both stood up a device, built a
    /// scene on it, traced into an image and read it back, and both are now one implementation that
    /// cannot disagree with itself.
    class VulkanRenderer final : public Renderer
    {
        /// Everything one scene is traced against — the world's, or a picture's in the interface.
        ///
        /// **The same three objects and the same three branches for both**, which is what lets an
        /// inventory doll be handed over by `Rtx::SceneUploader` exactly as a cell is: a slider
        /// drag places what it already built instead of building it again.
        ///
        /// **Three objects and not four.** `VisibilityPass` is shared, because nothing about it
        /// depends on which scene it traces — every texture array declares the same bindless layout,
        /// and identically defined layouts are compatible.
        struct ViewScene
        {
            std::unique_ptr<SceneAcceleration> mAcceleration;
            std::unique_ptr<SceneBuffers> mBuffers;
            std::unique_ptr<TextureArray> mTextures;

            /// One row per placement slot, made whole when the scene is built and kept across
            /// frames, with the rows the scene says changed rewritten by each placement.
            ///
            /// **Here rather than in either half**, because both need the same rows and each used
            /// to build its own: the acceleration structure for the transforms it places, the
            /// instance table for the motion the shader reads. A row carries a matrix inverse, and
            /// a nine-by-nine exterior is fifty thousand rows. Per scene, because a picture inside
            /// the interface places against rows of its own.
            std::vector<InstanceRecord> mRecords;

            /// Which of those `updateInstanceRecords` wrote this placement, cleared and refilled.
            ///
            /// **One list, read by both halves of a placement.** The acceleration structure's rows
            /// and the shading table's rows are derived from the same records, and each used to work
            /// out for itself which had changed. Two answers to one question is one of them being
            /// wrong, which is what put terrain a frame behind.
            std::vector<Index> mChangedRecords;

            /// Which revision of the mesh table the structures were built from, so `extendScene` can
            /// tell a scene that only gained textures from one that gained geometry too.
            ///
            /// Counted rather than sized, because a freed slot taken over by something else is a
            /// mesh arriving at a table that did not grow.
            std::uint64_t mBuiltMeshes = 0;
        };

        /// Everything one frame in flight owns: what it records into, what says it is done, what it
        /// measured, and what it may still be reading.
        ///
        /// **Two of these, and the CPU works one ahead of the GPU.** Frame N+1 is walked and placed
        /// while frame N is traced; what N+1 writes is this frame's copy of every table, and what N
        /// may still read is the other's. The frame after next takes this one's place, and waits
        /// its fence first.
        struct Frame
        {
            Frame(const Device& device, CommandPool& pool);

            /// The placement's commands and the trace's, submitted apart because a picture inside
            /// the interface is traced between the two and needs the first to have reached the
            /// queue. Only the second carries the fence: it is later on the queue, so its signal
            /// covers both.
            VkCommandBuffer mPlaceCommands = VK_NULL_HANDLE;
            VkCommandBuffer mCommands = VK_NULL_HANDLE;
            VkFence mFence = VK_NULL_HANDLE;

            /// Begun by a placement or a trace and not yet submitted with its fence.
            bool mBegun = false;

            /// Whether this frame's placement has been recorded, so a second placement before a
            /// trace closes the frame rather than recording over a submit in flight.
            bool mPlaced = false;

            /// Submitted with its fence and not yet waited for.
            bool mPending = false;

            /// Its own timer and its own count, because both are read after the fence, when the
            /// next frame is already writing its own.
            GpuTimer mTimer;
            Buffer mHitCount;

            /// What this frame may still be reading, destroyed when its fence says it is not.
            Graveyard mGraveyard;
            Reconstruction mReconstruction;

            /// The interface's own ring beside the frame's: it is drawn after the frame is submitted
            /// and fenced on its own, so its vertices are guarded by its own fence.
            VkCommandBuffer mGuiCommands = VK_NULL_HANDLE;
            VkFence mGuiFence = VK_NULL_HANDLE;
            bool mGuiPending = false;

            /// What the GUI is drawn out of, rewritten every frame it has anything in it and grown
            /// to the busiest frame so far. Host-visible device memory, so writing it is a memcpy
            /// and there is no staging copy and no transfer to record.
            HostBuffer mGuiVertices;
            Graveyard mGuiGraveyard;
        };

    public:
        /// Throws `Error` where this machine cannot run it. `createVulkanRenderer` is what turns
        /// that into a reason a caller can act on.
        explicit VulkanRenderer(const RendererOptions& options);
        ~VulkanRenderer() override;

        std::string describeDevice() const override;
        bool isValidating() const override;
        void resetHistory() override { mHistoryStale = true; }

        void setScene(std::uint32_t slot, const SceneDesc& scene, std::span<const TextureData> textures,
            const SeaState& sea) override;
        void extendScene(std::uint32_t slot, const SceneDesc& scene, std::span<const TextureData> arrived,
            const SeaState& sea) override;
        std::uint32_t getTextureCount(std::uint32_t slot) const override;
        void dropTextures(std::uint32_t slot, std::span<const std::uint32_t> textures) override;
        void placeScene(std::uint32_t slot, const SceneDesc& scene, const SeaState& sea) override;
        const SceneStats& getSceneStats() const override { return mStats; }
        void resize(std::uint32_t width, std::uint32_t height) override;
        FrameExtents getExtents() const override;
        Reconstruction renderFrame(const Shaders::VisibilityConstants& camera, const FrameOptions& options) override;
        std::optional<FrameResult> finishFrame() override;
        bool presentFrame() override;

        std::uint32_t addViewScene() override;
        void dropViewScene(std::uint32_t scene) override;

        std::uint32_t addGuiTexture(std::uint32_t width, std::uint32_t height) override;
        void writeGuiTexture(
            std::uint32_t texture, const GuiRegion& region, std::span<const std::uint8_t> rgba) override;
        void dropGuiTexture(std::uint32_t texture) override;
        void drawGui(std::span<const GuiVertex> vertices, std::span<const GuiBatch> batches) override;
        void traceGuiTexture(
            std::uint32_t texture, const Shaders::VisibilityConstants& camera, const GuiTraceOptions& options) override;
        void readGuiTexture(std::uint32_t texture, std::vector<std::uint8_t>& pixels) override;
        void readPixels(std::vector<std::uint8_t>& pixels) override;
        void readChannel(Channel channel, std::vector<float>& values) override;
        void takeValidationErrors(std::vector<std::string>& errors) override;

    private:
        /// The scene a slot names — `sWorld`'s, or a picture's. A slot nothing holds is a caller
        /// bug, so it is asserted rather than reported.
        ViewScene& sceneAt(std::uint32_t slot);
        const ViewScene& sceneAt(std::uint32_t slot) const;

        /// @param width, height what the frame is **presented** at. What it is traced at is the
        ///        upscaler's answer for that, or the same numbers where nothing upscales.
        void createTargets(std::uint32_t width, std::uint32_t height);

        /// Makes the picture-inside-the-interface chain at least this big, keeping whatever extent
        /// it already reached on either axis.
        void growViewTargets(std::uint32_t width, std::uint32_t height);

        Frame& frameSlot(std::uint64_t frame) { return mFrames[frame % sFrameSlots]; }

        /// The frame being recorded, begun if it was not: the frame that last used its slot is
        /// waited for, its fence reset, its timer and hit count cleared.
        Frame& beginFrame();

        /// Submits a begun frame that will not be traced — a placement followed by another — so
        /// its fence exists and its slot comes round again.
        void closeFrame();

        /// Submits what a frame recorded, under its own fence, and counts it as in flight.
        void submitFrame(Frame& frame);

        /// Waits for the oldest frame in flight and reads back what it measured.
        FrameResult finishOldest();

        /// Waits until `frame` is finished, where it was ever submitted.
        void finishThrough(std::uint64_t frame);

        /// Waits for every frame in flight. What an arrival, a rebuild, a resize and a picture
        /// inside the interface do first.
        void finishFrames();

        /// Destroys what every frame is holding, whether or not its slot ever comes round again.
        ///
        /// **After `waitIdle`, and only where something buried is about to lose its owner.** A room
        /// is a pointer into a scene's structure storage and a structure stands in that storage, so
        /// a frame that placed a scene and was never traced would give both back to a scene that no
        /// longer exists. `finishFrames` cannot reach that frame: it was never submitted.
        void emptyGraveyards();

        // Declaration order is destruction order reversed, and everything below the device is built
        // on it.
        Instance mInstance;
        /// What the finished picture is encoded into, and so what the GUI pass is compiled against.
        /// Four bytes a pixel and not display-encoded by the hardware: the tone curve has already
        /// run by the time anything is written here.
        static constexpr VkFormat sTargetFormat = VK_FORMAT_R8G8B8A8_UNORM;

        Device mDevice;
        CommandPool mPool;

        std::array<Frame, sFrameSlots> mFrames;

        /// The next frame to record and the next to finish. Everything from `mFinished` to `mFrame`
        /// is in flight, and there are never more of those than there are slots.
        std::uint64_t mFrame = 0;
        std::uint64_t mFinished = 0;

        /// The interface's ring runs on its own count: a menu is drawn on frames with no world.
        std::uint64_t mGuiFrame = 0;

        /// Which copy of the world's tables the last placement wrote — what a frame traces, what
        /// a picture inside the interface traces, and the copy the next placement leaves alone.
        ///
        /// **A placement's parity and not a frame's**, because a frame need not place: a test that
        /// traces the same placement twice reads the same copy twice, and the copy a placement is
        /// about to write is guarded by the frame that last traced it, not by the frame count.
        std::uint32_t mWorldSlot = 0;

        /// The last frame that traced each copy of the world's tables, or `sNeverRead`.
        static constexpr std::uint64_t sNeverRead = ~std::uint64_t{ 0 };
        std::array<std::uint64_t, sFrameSlots> mReadBy{ sNeverRead, sNeverRead };

        std::filesystem::path mShaderDirectory;

        /// Whether the trace this builds counts its hits. `RendererOptions::mCountHits` says why the
        /// game's does not.
        bool mCountHits = false;

        /// Fixed at construction: an upscaler is brought up once, and there is nothing to switch
        /// to at runtime that would not mean rebuilding every target anyway.
        Upscale mUpscale = Upscale::Off;
        Preset mPreset = Preset::Default;

        /// Whether the next frame has to be reconstructed without a past. Set by `resetHistory` and
        /// spent by the next frame that reconstructs from one, which is not always the one after.
        bool mHistoryStale = false;

        /// When the last frame was recorded, so the next can say how long ago that was.
        ///
        /// **Measured here rather than asked of the caller.** What the upscaler wants is the
        /// interval between the frames it is reconstructing across, and this is the function those
        /// frames pass through — a number handed in instead could be forgotten by one caller,
        /// stale in another, and wrong in both without anything saying so.
        std::optional<std::chrono::steady_clock::time_point> mLastFrameAt;

        /// What the trace runs at, and so what every G-buffer channel and the composite are sized
        /// to. Equal to the output extent wherever nothing upscales.
        std::uint32_t mRenderWidth = 0;
        std::uint32_t mRenderHeight = 0;

        std::uint32_t mOutputWidth = 0;
        std::uint32_t mOutputHeight = 0;

        /// The composite's output at the render extent: one frame in linear radiance, before
        /// anything upscales it and before the display curve.
        std::unique_ptr<Image> mColour;

        /// The frame as bytes at the output extent, which is what anything outside this reads.
        ///
        /// **Two of them, swapped by every present.** A present's blit reads its image long after
        /// the call that queued it returned — it waits the acquire semaphore, which under FIFO the
        /// presentation engine signals when it lets that swapchain image go — and the discard a
        /// frame opens with is sourced at `TOP_OF_PIPE`, which waits for nothing. One image would
        /// have each frame rewriting what the last is still being read out of, and no barrier can
        /// order that: a source scope does not reach across a submit.
        std::unique_ptr<Image> mTarget;

        /// The other one. Which of the two is which changes every present, so neither is special.
        std::unique_ptr<Image> mSpare;

        /// The one the last present read — which is `mSpare`, since the swap is what put it there —
        /// or null where nothing has presented at all. Named separately because that null is the
        /// whole question `readPixels` asks, and a headless run never answers it.
        const Image* mPresented = nullptr;

        /// The running sum a reference is built out of, and null until a frame asks for one.
        ///
        /// **Not a history, and nothing here reprojects.** The denoiser's past is `mHistoryStale`,
        /// `mPreviousCamera` and the image pairs `AccumulatePass` keeps. This is a plain per-pixel
        /// total over however many frames the caller asked to average, so a world that moved under
        /// it is what it is a sum of rather than a reason to drop it.
        ///
        /// **Whether it exists is also whether anything has written it**, because the frame that
        /// makes one is the frame that fills it: the first write needs no contents and nothing to
        /// wait on, and every one after reads what the last left — a hazard across submits that the
        /// fence orders and does not make visible.
        std::unique_ptr<Image> mSum;

        /// What every `GBuffer` here is shaped by — one description, however many of them the
        /// frame's size brings and takes away. `GBufferLayout` says why the channels have a set.
        ///
        /// **Declared before both of them**, because the trace's pipeline names it when it is built
        /// and every buffer allocates from it.
        GBufferLayout mChannelLayout;

        /// What the trace writes and the composite reads: one frame's light, still in pieces.
        std::unique_ptr<GBuffer> mChannels;

        /// The camera the last frame was traced with, for reprojecting this one against.
        ///
        /// Its basis is all zero until a frame has been traced, and after a resize or a new scene —
        /// which the shader reads as "there is no previous frame" and answers with no motion at all.
        Shaders::VisibilityConstants mPreviousCamera{};

        /// The world's, which is one of these like any other: what `sWorld` names.
        ViewScene mWorld;

        std::unique_ptr<VisibilityPass> mPass;
        SceneStats mStats;

        /// Held by value rather than built with the scene, because they depend on neither the
        /// scene nor the size of the image: what they read is pushed at record time. The filter is
        /// not const only because it keeps a channel the size of the frame.
        AccumulatePass mAccumulate;
        AtrousPass mFilter;

        /// The same wavelet over the pictures inside the interface, which need it for the same
        /// reason a frame does: one bounce a pixel is noisy, and a doll is looked at closely.
        AccumulatePass mViewAccumulate;
        AtrousPass mViewFilter;

        CompositePass mComposite;
        BloomPass mBloom;

        /// **One sea for everything traced**, the doll and the map included: the water is not a
        /// property of a scene, so it is synthesised once a frame here rather than held per scene.
        WavePass mWaves;

        /// **One field for everything traced, drawn once for the life of the device.** Nothing about
        /// it turns on the weather or the cell — those decide the extinction and the layer's height,
        /// which are numbers the shader already has.
        FogTile mFog;
        ExposurePass mExposure;
        /// **Held like `mPass` and for its reason**: it samples the scene's textures, so it needs a
        /// layout that only a scene brings, and the layout every scene brings is the same one.
        std::unique_ptr<TonePass> mTone;
        GuiPass mGuiPass;
        GuiTextures mGuiTextures;

        /// The batches, resolved from slots to what the pass wants. Kept so that a frame of GUI
        /// allocates nothing.
        std::vector<GuiDraw> mGuiDraws;

        /// What a picture inside the interface is traced through: a map tile, the inventory doll,
        /// the race preview. Null until something asks for one.
        ///
        /// **Its own chain and not the frame's.** Nothing here upscales, averages or measures an
        /// exposure — a doll is a still picture of a subject rather than a frame in a sequence — and
        /// borrowing the frame's images would mean resizing them away from the frame and back
        /// between two of them.
        ///
        /// **Grown to the largest picture asked for and never shrunk.** There are three or four
        /// sizes in the whole game and every pass below takes the extent it is dispatched over, so
        /// a smaller picture uses a corner of a larger one's images rather than rebuilding them.
        /// Scenes belonging to pictures rather than to the world, by slot.
        std::vector<std::unique_ptr<ViewScene>> mViewScenes;
        std::vector<std::uint32_t> mFreeViewScenes;

        std::unique_ptr<GBuffer> mViewChannels;
        std::unique_ptr<Image> mViewColour;
        std::unique_ptr<Image> mViewTarget;
        std::uint32_t mViewWidth = 0;
        std::uint32_t mViewHeight = 0;

        /// Null where nothing asked for a window.
        ///
        /// **Last, so it is destroyed first**, which is not a detail: its command buffers still hold
        /// recordings that blit out of `mTarget`, and destroying that image while a recording names
        /// it is `VUID-vkDestroyImage-image-01000`. Declared beside the device — where its lifetime
        /// reads as belonging — it outlived the image instead, and the layers said so on the way
        /// out of a two-hundred-frame run.
        std::unique_ptr<Presenter> mPresenter;

#ifdef OPENMW_RTX_DLSS
        /// A share of the process's NGX runtime, held for as long as this renderer upscales, and
        /// null where it does not. `describeDevice` takes a share of its own to answer with, which
        /// is this same object wherever this one is holding it.
        /// NGX, where this renderer was asked to upscale. **Owned outright and null otherwise** —
        /// built in the constructor, destroyed with the renderer, and the only one in the process.
        std::unique_ptr<Dlss> mNgx;

        /// Ray Reconstruction, built for one pair of resolutions and so rebuilt by every resize.
        std::unique_ptr<DlssPass> mUpscaler;

        /// What it writes: the frame at the output extent, still in linear radiance.
        std::unique_ptr<Image> mUpscaled;

#endif
    };

    /// Builds a Vulkan renderer, or nothing where this machine has no driver that qualifies.
    std::unique_ptr<Renderer> createVulkanRenderer(const RendererOptions& options, std::string& reason);
}
