#pragma once

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
#include "buffer.hpp"
#include "commands.hpp"
#include "compositepass.hpp"
#include "device.hpp"
#include "exposurepass.hpp"
#include "gputimer.hpp"
#include "guipass.hpp"
#include "guitextures.hpp"
#include "instance.hpp"
#include "tonepass.hpp"

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

            /// Which revision of the mesh table the structures were built from, so `extendScene` can
            /// tell a scene that only gained textures from one that gained geometry too.
            ///
            /// Counted rather than sized, because a freed slot taken over by something else is a
            /// mesh arriving at a table that did not grow.
            std::uint64_t mBuiltMeshes = 0;
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
        FrameResult renderFrame(const Shaders::VisibilityConstants& camera, const FrameOptions& options) override;
        SharedFrame shareFrame() override;
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

        // Declaration order is destruction order reversed, and everything below the device is built
        // on it.
        Instance mInstance;
        /// What the finished picture is encoded into, and so what the GUI pass is compiled against.
        /// Four bytes a pixel and not display-encoded by the hardware: the tone curve has already
        /// run by the time anything is written here.
        static constexpr VkFormat sTargetFormat = VK_FORMAT_R8G8B8A8_UNORM;

        Device mDevice;
        CommandPool mPool;

        /// Where the device spent the frame, written by the command stream itself.
        ///
        /// **One timer across all three submits.** A frame that moved places its structures in two
        /// submits of its own before the one that draws it, and all three are the same frame's cost;
        /// the timer is opened once at the top of `placeScene` — or of `renderFrame`, where nothing
        /// moved — and read at the end of the frame.
        GpuTimer mTimer;

        /// Whether `placeScene` has already opened this frame's timer, so `renderFrame` adds to that
        /// report rather than starting a second one and throwing the builds away.
        bool mTimed = false;

        std::filesystem::path mShaderDirectory;

        /// Whether the trace this builds counts its hits. `RendererOptions::mCountHits` says why the
        /// game's does not.
        bool mCountHits = false;

        /// Fixed at construction: an upscaler is brought up once, and there is nothing to switch
        /// to at runtime that would not mean rebuilding every target anyway.
        Upscale mUpscale = Upscale::Off;
        Preset mPreset = Preset::Default;

        /// Whether the next frame has to be reconstructed without a past. Set by `resetHistory` and
        /// spent by the frame that follows it.
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
        std::unique_ptr<Image> mTarget;

        /// The running sum, and null until a frame asks to be averaged into one.
        ///
        /// **Whether it exists is also whether anything has written it**, because the frame that
        /// makes one is the frame that fills it: the first write needs no contents and nothing to
        /// wait on, and every one after reads what the last left — a hazard across submits that the
        /// fence orders and does not make visible.
        std::unique_ptr<Image> mHistory;

        /// What the trace writes and the composite reads: one frame's light, still in pieces.
        std::unique_ptr<GBuffer> mChannels;

        /// The camera the last frame was traced with, for reprojecting this one against.
        ///
        /// Its basis is all zero until a frame has been traced, and after a resize or a new scene —
        /// which the shader reads as "there is no previous frame" and answers with no motion at all.
        Shaders::VisibilityConstants mPreviousCamera{};

        Buffer mHitCount;

        /// The world's, which is one of these like any other: what `sWorld` names.
        ViewScene mWorld;

        std::unique_ptr<VisibilityPass> mPass;
        SceneStats mStats;

        /// One row per placement slot, remade whenever the world is built or moved, and read by both
        /// halves of it.
        ///
        /// **Here rather than in either of them**, because both need the same rows and each used to
        /// build its own: the acceleration structure for the transforms it places, the instance table
        /// for the motion the shader reads. A row carries a matrix inverse, and a nine-by-nine
        /// exterior is fifty thousand rows. Refilled in place; a frame path does not allocate.
        std::vector<InstanceRecord> mRecordScratch;

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
        ExposurePass mExposure;
        TonePass mTone;
        GuiPass mGuiPass;
        GuiTextures mGuiTextures;

        /// What the GUI is drawn out of, rewritten every frame it has anything in it and grown to
        /// the busiest frame so far. Host-visible device memory, so writing it is a memcpy and
        /// there is no staging copy and no transfer to record.
        HostBuffer mGuiVertices;

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
