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
#include <components/rtx/slots.hpp>

#include "bloompass.hpp"
#include "buffer.hpp"
#include "commands.hpp"
#include "compositepass.hpp"
#include "device.hpp"
#include "exposurepass.hpp"
#include "fogvolume.hpp"
#include "framering.hpp"
#include "frameslots.hpp"
#include "gputimer.hpp"
#include "graveyard.hpp"
#include "guipass.hpp"
#include "guitextures.hpp"
#include "handles.hpp"
#include "instance.hpp"
#include "placing.hpp"
#include "presenttargets.hpp"
#include "skinpass.hpp"
#include "spritepasses.hpp"
#include "tonepass.hpp"
#include "tracechain.hpp"
#include "visibilitypass.hpp"
#include "wavepass.hpp"

namespace Rtx
{
#ifdef OPENMW_RTX_DLSS
    class Dlss;
    class DlssPass;
#endif
    class Image;

    class Presenter;
    class SceneAcceleration;
    class SceneBuffers;
    class SkinTables;
    class TextureArray;

    /// `Renderer` over Vulkan.
    class VulkanRenderer final : public Renderer
    {
        static constexpr std::uint64_t sNeverRead = ~std::uint64_t{ 0 };

        /// Everything one scene is traced against — the world's, or a picture's in the interface —
        /// the same objects for both, which is what lets `Rtx::SceneUploader` hand a doll over
        /// exactly as a cell. `VisibilityPass` and `SkinPass` are shared: every texture array
        /// declares the same bindless layout, and identically defined layouts are compatible.
        struct ViewScene
        {
            std::unique_ptr<SceneAcceleration> mAcceleration;
            std::unique_ptr<SceneBuffers> mBuffers;
            std::unique_ptr<TextureArray> mTextures;

            /// What this scene's skinned bodies and morphed faces are posed from.
            std::unique_ptr<SkinTables> mSkinTables;

            /// One row per placement slot, made whole when the scene is built and kept across
            /// frames, with the rows the scene says changed rewritten by each placement. Here
            /// rather than in either half, because the acceleration structure and the instance
            /// table need the same rows, and each building its own was fifty thousand matrix
            /// inverses on a nine-by-nine exterior.
            std::vector<InstanceRecord> mRecords;

            /// Which of those `updateInstanceRecords` wrote this placement, cleared and refilled.
            /// One list read by both halves, because two answers to which changed is one of them
            /// wrong and terrain a frame behind.
            std::vector<Index> mChangedRecords;

            /// Which revision of the mesh table the structures were built from, so `extendScene` can
            /// tell a scene that only gained textures from one that gained geometry too. A revision
            /// and not a size, because a freed slot taken over is a mesh arriving at a table that
            /// did not grow.
            std::uint64_t mBuiltMeshes = 0;

            /// The revision of the whole structure this was built from: what `describeHeld` answers
            /// and an uploader appends against.
            std::uint64_t mBuiltStructure = 0;

            /// Which copy of the tables the last placement wrote — what a trace of this scene reads,
            /// and the copy the next placement leaves alone. A placement's parity and not a frame's,
            /// because a frame need not place; per scene, so a doll redrawn on consecutive frames
            /// places exactly as the world does.
            FrameSlot mSlot;

            /// The last frame that traced each copy, or `sNeverRead`.
            std::array<std::uint64_t, sFrameSlots> mReadBy{ sNeverRead, sNeverRead };
        };

    public:
        /// Throws `Error` where this machine cannot run it. `createVulkanRenderer` is what turns
        /// that into a reason a caller can act on.
        explicit VulkanRenderer(const RendererOptions& options);
        ~VulkanRenderer() override;

        std::string describeDevice() const override;
        bool isValidating() const override;
        void resetHistory() override { mDenoiserStale = mAirStale = true; }

        void setScene(SceneSlot slot, const SceneDesc& scene, std::span<const TextureData> textures) override;
        void extendScene(SceneSlot slot, const SceneDesc& scene, std::span<const TextureData> arrived) override;
        SceneHeld describeHeld(SceneSlot slot) const override;
        void dropTextures(SceneSlot slot, std::span<const Index> textures) override;
        void placeScene(SceneSlot slot, const SceneDesc& scene) override;
        const SceneStats& getSceneStats() const override { return mStats; }
        MemoryReport getMemoryReport() const override;
        void resize(std::uint32_t width, std::uint32_t height) override;
        void setUpscale(Upscale upscale) override;
        Upscale getUpscale() const override { return mUpscaling.mMode; }

        void setSea(const SeaState& sea) override;
        void setVerticalSync(SDLUtil::VSyncMode mode) override;
        FrameExtents getExtents() const override;
        Reconstruction renderFrame(const Shaders::VisibilityConstants& camera, const FrameOptions& options) override;
        std::optional<FrameResult> finishFrame() override;
        bool presentFrame() override;

        SceneSlot addViewScene() override;
        void dropViewScene(SceneSlot scene) override;

        GuiSlot addGuiTexture(std::uint32_t width, std::uint32_t height) override;
        std::span<std::uint8_t> lendGuiTexture(GuiSlot texture, const GuiRegion& region) override;
        void sendGuiTexture(GuiSlot texture) override;
        void dropGuiTexture(GuiSlot texture) override;
        void drawGui(std::span<const GuiVertex> vertices, std::span<const GuiBatch> batches) override;
        void traceGuiTexture(
            GuiSlot texture, const Shaders::VisibilityConstants& camera, const GuiTraceOptions& options) override;
        bool takeGuiCopy(GuiSlot texture, std::span<std::uint8_t> into) override;
        void finishGuiTraces() override;
        void readGuiTexture(GuiSlot texture, std::vector<std::uint8_t>& pixels) override;
        void readPixels(std::vector<std::uint8_t>& pixels) override;
        void readChannel(Channel channel, std::vector<float>& values) override;
        void readFrameImage(FrameImage image, std::vector<float>& values) override;
        void takeValidationErrors(std::vector<std::string>& errors) override;

    private:
        /// Widens a channel stored as bytes or as halves on the way out.
        void readImage(const Image& image, std::vector<float>& values);

        /// The scene a slot names — the world's, or a picture's. A slot nothing holds is a caller
        /// bug, so it is asserted rather than reported.
        const ViewScene& sceneAt(SceneSlot slot) const;
        ViewScene& sceneAt(SceneSlot slot);

        /// What the trace reads a scene through, for the copy its last placement wrote. One
        /// description for a frame and for a picture inside the interface, which differ in the fog
        /// volume they march and in nothing else.
        VisibilityInputs describeInputs(const ViewScene& held, const FogVolume* volume, std::uint32_t rayMask) const;

        /// The frame's camera as its trace will sample it: what the caller wrote, plus every field
        /// only the renderer can fill. The one place a sampled camera is made, so no field goes
        /// missing.
        Shaders::VisibilityConstants sampleCamera(
            const Shaders::VisibilityConstants& camera, const Reconstruction& reconstruction) const;

        /// Everything a placement of `held` is, recorded and written where `placing` says. True
        /// where anything was recorded, which is whether its command buffer is worth submitting.
        /// What differs between the world's placement and a picture's is around this and not in it.
        static bool recordPlacement(
            const SkinPass& skin, ViewScene& held, const SceneDesc& scene, const Placing& placing);

        /// Reads into `mStats` what a placement can have moved, which is every figure but the three
        /// a build settles — one of which is a loop over every texture, and a placement runs on the
        /// frame path.
        void readPlacedStats(const ViewScene& held);

        /// Reads all of `mStats`, for a scene that has just been built or extended.
        void readStats(const ViewScene& held);

        /// @param width, height what the frame is presented at. What it is traced at is the
        ///        upscaler's answer for that, or the same numbers where nothing upscales.
        void createTargets(std::uint32_t width, std::uint32_t height);

        /// Brings the upscaler's runtime up if it is not already, and throws where it cannot be.
        void startUpscaler();

        /// Whether a frame is upscaled: a runtime that is up and a mode that wants one. The
        /// runtime outlives a mode being turned off, because raising it again costs a quarter of a
        /// second. The one answer the build decides, so that no reader of the three members below
        /// has to be conditionally compiled.
        bool upscaling() const;

        /// Makes the picture-inside-the-interface chain at least this big, and the byte image the
        /// interface is handed with it.
        void growViewTargets(std::uint32_t width, std::uint32_t height);

        // Declaration order is destruction order reversed, and everything below the device is built
        // on it.
        Instance mInstance;

        Device mDevice;
        CommandPool mPool;

        /// The interface's ring runs on its own count: a menu is drawn on frames with no world.
        std::uint64_t mGuiFrame = 0;

        std::filesystem::path mShaderDirectory;

        /// Whether the trace this builds counts its hits. `RendererOptions::mCountHits` says why the
        /// game's does not.
        bool mCountHits = false;

        /// Whether it counts the see-through surfaces each primary ray crosses.
        /// `RendererOptions::mCountCrossings` says why that is a switch of its own.
        bool mCountCrossings = false;

        /// The frames in flight, their fences and what each may still be reading. After the two
        /// counters, which it borrows.
        FrameRing mRing{ mDevice, mPool, mCountHits, mCountCrossings };

        /// What the frames are traced under. Changing the mode rebuilds every target, which is
        /// what `setUpscale` is for and why it is a setting rather than a frame option.
        Upscaling mUpscaling;

        /// Whether the next frame has to be reconstructed without a past. Set by `resetHistory` and
        /// spent by the next frame that reconstructs from one, which is not always the one after.
        bool mDenoiserStale = false;

        /// The same for the fog volume, which keeps a past of its own. Two flags because two
        /// histories are spent by different frames: the denoisers run only where a frame
        /// reconstructs, so their signal has to survive a frame that runs none, while the air is
        /// filled by every trace, so its signal is spent by the very next one.
        bool mAirStale = false;

        /// When the last frame was recorded, so the next can say how long ago that was. Measured
        /// here rather than asked of the caller, because this is the function the frames the
        /// upscaler reconstructs across pass through.
        std::optional<std::chrono::steady_clock::time_point> mLastFrameAt;

        std::uint32_t mOutputWidth = 0;
        std::uint32_t mOutputHeight = 0;

        /// The frame as bytes at the output extent, which is what anything outside this reads: two
        /// images, swapped by every present, and the one the last present read. `PresentTargets`
        /// says why there are two.
        PresentTargets mTargets;

        /// The running sum a reference is built out of, and null until a frame asks for one. Not a
        /// history and nothing here reprojects: a plain per-pixel total over however many frames
        /// the caller asked to average. Whether it exists is also whether anything has written it,
        /// because the frame that makes one fills it.
        std::unique_ptr<Image> mSum;

        /// What every `GBuffer` here is shaped by — one description, however many of them the
        /// frame's size brings and takes away. Declared before both chains, which allocate from it.
        SetLayout mChannelLayout;

        /// The same for the air, which is a camera's the way the channels are. Declared before both
        /// chains for the reason `mChannelLayout` is.
        SetLayout mFogVolumeLayout;

        /// What the frame is traced into, at the render extent — which is the output extent
        /// wherever nothing upscales.
        TraceChain mFrame;

        /// What a picture inside the interface is traced into: a map tile, the inventory doll, the
        /// race preview. Its own chain and not the frame's, because borrowing the frame's images
        /// would mean resizing them away from the frame and back between two of them.
        TraceChain mView;

        /// The camera the last frame was traced with, for reprojecting this one against. Its basis
        /// is all zero until a frame is traced, and after a resize or a new scene, which the shader
        /// reads as "there is no previous frame" and answers with no motion at all.
        Shaders::VisibilityConstants mPreviousCamera{};

        /// The world's, which is one of these like any other: what `SceneSlot::world` names.
        ViewScene mWorld;

        std::unique_ptr<VisibilityPass> mPass;
        SceneStats mStats;

        CompositePass mComposite;
        BloomPass mBloom;

        /// One sea for everything traced, the doll and the map included: the water is not a
        /// property of a scene, so it is synthesised once a frame here rather than held per scene.
        WavePass mWaves;

        /// One field for everything traced, drawn once for the life of the device. Nothing about
        /// it turns on the weather or the cell — those decide the extinction and the layer's height,
        /// which are numbers the shader already has.
        FogTile mFog;
        ExposurePass mExposure;

        /// One pass for everything posed, the doll included: what differs per scene is the
        /// tables, which each `ViewScene` holds.
        SkinPass mSkinPass;

        /// One pass for everything binned, for the same reason: what differs per scene is the
        /// tables, and the camera arrives with the frame.
        SpriteBinPass mSpriteBin;

        /// And one for everything shaded, which runs ahead of the bin over the same tables.
        SpriteShadePass mSpriteShade;

        /// An empty sprite tiles' list, for a camera that draws no sprites and so binned none.
        /// `VisibilityInputs::mSpriteList` says why one buffer serves every extent.
        Buffer mNoSprites;

        /// What a picture inside the interface sums its census into, which nothing reads: the hit
        /// count and the crossings are the frame's, and a picture traced beside it must not add to
        /// them.
        Buffer mViewCounts;

        /// Held like `mPass` and for its reason: it samples the scene's textures, so it needs a
        /// layout that only a scene brings, and the layout every scene brings is the same one.
        std::unique_ptr<TonePass> mTone;

        /// The interface: `GuiTextures` holds the part with a rule, and the rest is a pipeline, a
        /// scratch vector and a counter with nothing binding them.
        GuiPass mGuiPass;
        GuiTextures mGuiTextures;

        /// The batches, resolved from slots to what the pass wants. Kept so that a frame of GUI
        /// allocates nothing.
        std::vector<GuiDraw> mGuiDraws;

        /// Scenes belonging to pictures rather than to the world, by slot, and the slots nothing
        /// holds.
        std::vector<std::unique_ptr<ViewScene>> mViewScenes;
        SlotPool mFreeViewScenes;

        /// The picture as bytes, which is what the interface's texture is copied out of. Null
        /// until something asks for a picture, and grown with `mView`.
        std::unique_ptr<Image> mViewTarget;

        /// Null where nothing asked for a window. Last, so it is destroyed first: its command
        /// buffers still hold recordings that blit out of `mTarget`, and destroying that image while
        /// a recording names it is `VUID-vkDestroyImage-image-01000`.
        std::unique_ptr<Presenter> mPresenter;

#ifdef OPENMW_RTX_DLSS
        /// NGX, raised by `startUpscaler` the first time a mode wants one and null otherwise. It
        /// outlives a mode being turned off, so `upscaling` and not this says whether a frame is
        /// upscaled; `describeDevice` reports from `Dlss::probe`, which asks the device without
        /// standing a runtime up.
        std::unique_ptr<Dlss> mNgx;

        /// Ray Reconstruction, built for one pair of resolutions and so rebuilt by every resize.
        std::unique_ptr<DlssPass> mUpscaler;

        /// What it writes: the frame at the output extent, still in linear radiance.
        std::unique_ptr<Image> mUpscaled;

#endif
    };
}
