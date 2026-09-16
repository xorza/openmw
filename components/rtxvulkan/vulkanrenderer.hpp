#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <components/rtx/frameimage.hpp>
#include <components/rtx/guirenderer.hpp>
#include <components/rtx/memoryreport.hpp>
#include <components/rtx/reconstruction.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/shaders/visibility.h>
#include <components/rtx/slot.hpp>
#include <components/rtx/slots.hpp>
#include <components/rtx/texturedata.hpp>
#include <components/rtx/upscale.hpp>
#include <components/rtx/wavespectrum.hpp>
#include <components/sdlutil/vsyncmode.hpp>

#include "buffer.hpp"
#include "commands.hpp"
#include "compositepass.hpp"
#include "device.hpp"
#include "displaychain.hpp"
#include "fogvolume.hpp"
#include "framering.hpp"
#include "frameslots.hpp"
#include "guipass.hpp"
#include "guitextures.hpp"
#include "handles.hpp"
#include "image.hpp"
#include "instance.hpp"
#include "presenttargets.hpp"
#include "ripplepass.hpp"
#include "skinpass.hpp"
#include "spritepasses.hpp"
#include "stresspass.hpp"
#include "tracechain.hpp"
#include "visibilitypass.hpp"
#include "wavepass.hpp"

namespace Rtx
{
    class DeviceScene;
    class Presenter;
    class Upscaler;

    /// `Renderer` over Vulkan.
    class VulkanRenderer final : public Renderer
    {
    public:
        /// Throws `Unsupported` where this machine cannot run it and `Error` where it should have.
        /// `createVulkanRenderer` is how a host makes one.
        explicit VulkanRenderer(const RendererOptions& options);
        ~VulkanRenderer() override;

        std::string describeDevice() const override;
        bool isValidating() const override;
        void resetHistory() override
        {
            mDenoiserStale = mAirStale = true;
            mRipples.reset();
        }

        void setScene(SceneSlot slot, const SceneDesc& scene, std::span<const TextureData> textures) override;
        void extendScene(SceneSlot slot, const SceneDesc& scene, std::span<const TextureData> arrived) override;
        SceneHeld describeHeld(SceneSlot slot) const override;
        void dropTextures(SceneSlot slot, std::span<const Index> textures) override;
        void placeScene(SceneSlot slot, const SceneDesc& scene) override;
        const SceneStats& getSceneStats() const override { return mStats; }
        MemoryReport getMemoryReport() const override;
        void resize(std::uint32_t width, std::uint32_t height) override;
        void setUpscale(Upscale upscale) override;
        Upscale getUpscale() const override { return mProfile.mUpscaling.mMode; }

        void setVerticalSync(SDLUtil::VSyncMode mode) override;
        FrameExtents getExtents() const override;
        Reconstruction renderFrame(const Shaders::VisibilityConstants& camera, const FrameOptions& options) override;
        std::optional<FrameResult> finishFrame() override;
        std::optional<FrameResult> collectFrame() override;
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
        void readPixels(std::vector<std::uint8_t>& pixels) override;

        /// What a test asks of this backend and a game never does. None of the five is on a frame
        /// path: each that reads submits a copy and waits for it, so none is const.

        /// The sea every scene is traced with, `SeaState{}` until told. Uploads a spectrum and
        /// waits the frames in flight out first.
        void setSea(const SeaState& sea);

        /// Copies one of the last frame's g-buffer channels into `values`, tightly packed, widened
        /// to floats whatever the channel holds. The frame's, never a view scene's.
        void readChannel(Channel channel, std::vector<float>& values);

        /// The same for the composite's own output, which no channel holds: the frame a measurement
        /// is taken on, where `readPixels` gives the one a display would show.
        void readComposite(std::vector<float>& values);

        /// The whole of a GUI texture as the device holds it, four bytes a pixel, tightly packed,
        /// row zero first.
        void readGuiTexture(GuiSlot texture, std::vector<std::uint8_t>& pixels);

        /// Moves whatever the API has complained about since the last call into `errors`, so that
        /// clearing before a test and reading after it are the same call.
        void takeValidationErrors(std::vector<std::string>& errors);

    private:
        /// Widens a channel stored as bytes or as halves on the way out.
        void readImage(const Image& image, std::vector<float>& values);

        /// The image this frame writes, with the present that last read it waited for — once per
        /// frame, at the first of the trace and the interface to want it.
        Image& claimTarget();

        /// Where the scene a slot names sits — the world's, or a picture's — which holds null until
        /// `setScene` fills it. A slot nothing was ever given is a caller bug, so it is asserted.
        const std::unique_ptr<DeviceScene>& slotAt(SceneSlot slot) const;
        std::unique_ptr<DeviceScene>& slotAt(SceneSlot slot);

        /// The scene a slot names, which `setScene` has filled: asked of an empty slot is a caller
        /// bug, so it is asserted rather than reported.
        const DeviceScene& sceneAt(SceneSlot slot) const;
        DeviceScene& sceneAt(SceneSlot slot);

        /// What the trace reads a scene through, for the copy its last placement wrote. One
        /// description for a frame and for a picture inside the interface, which differ in the
        /// chain they trace into and in the image the puffs are composited over.
        VisibilityInputs describeInputs(
            const DeviceScene& held, const TraceChain& chain, std::uint32_t rayMask, const Image& shown) const;

        /// A camera as its trace will sample it: what the caller wrote, plus every field only the
        /// renderer can fill — the jitter, what the scene behind it holds, and where the eye was.
        /// The one place a sampled camera is made, for a frame and for a picture inside the
        /// interface alike, so no field goes missing from either.
        ///
        /// @param previous the camera the last frame was traced with, to reproject against, or
        ///        null for a picture, which has no frame before it.
        Shaders::VisibilityConstants sampleCamera(const Shaders::VisibilityConstants& camera, const DeviceScene& scene,
            const Reconstruction& reconstruction, const Shaders::VisibilityConstants* previous) const;

        /// @param width, height what the frame is presented at. What it is traced at is the
        ///        upscaler's answer for that, or the same numbers where nothing upscales.
        void createTargets(std::uint32_t width, std::uint32_t height);

        /// Brings the upscaler's runtime up if it is not already, and throws where it cannot be.
        void startUpscaler();

        /// Everything the queue was given and everything waiting to be given it, finished, and
        /// everything buried let go: what a rebuild, a resize and a scene going away do before
        /// what they replace can go. In the one order that is right — a deferred batch first,
        /// because it rides the next submit and nothing else will make one; the frames in flight,
        /// so the ring's account is settled; the device, for the interface's and the presenter's
        /// submits the ring does not count; and the graveyard last, once nothing can be reading.
        void drain();

        /// Whether a frame is upscaled: a runtime that is up and a mode that wants one. The
        /// runtime outlives a mode being turned off, because raising it again costs a quarter of a
        /// second.
        bool upscaling() const;

        /// Makes the picture-inside-the-interface chain at least this big, and the byte image the
        /// interface is handed with it.
        void growViewTargets(std::uint32_t width, std::uint32_t height);

        // Declaration order is destruction order reversed, and everything below the device is built
        // on it.
        Instance mInstance;

        Device mDevice;

        /// The interface's ring runs on its own count: a menu is drawn on frames with no world.
        std::uint64_t mGuiFrame = 0;

        /// Whether the trace this builds counts its hits. `RendererOptions::mCountHits` says why the
        /// game's does not.
        bool mCountHits = false;

        /// What the run decided once, read where each knob is used: how wide both chains store
        /// their radiance, how long the queue is held, and what the frames are traced under —
        /// `mUpscaling` as it was handed over, and then whatever `setUpscale` moved it to, which
        /// rebuilds every target and is why the mode is a setting rather than a frame option. What
        /// a frame carries — the reconstruction request, the exposure, the delight, the sample —
        /// is read off the frame's own blocks instead, which is where a frame that asks otherwise
        /// says so.
        RenderProfile mProfile;

        /// The frames in flight and what each came to. After the counters it is handed.
        FrameRing mRing{ mDevice, mCountHits };

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

        /// The running sum a reference is built out of, and empty until a frame asks for one. Not
        /// a history and nothing here reprojects: a plain per-pixel total over however many frames
        /// the caller asked to average. Whether it exists is also whether anything has written it,
        /// because the frame that makes one fills it.
        Image mSum;

        /// What every `GBuffer` here is shaped by — one description, however many of them the
        /// frame's size brings and takes away. Declared before both chains, which allocate from it.
        SetLayout mChannelLayout;

        /// The same for the air, which is a camera's the way the channels are. Declared before both
        /// chains for the reason `mChannelLayout` is.
        SetLayout mFogVolumeLayout;

        /// And what every scene's texture array is shaped by, so one pass samples any scene's set.
        SetLayout mTextureLayout;

        /// The passes every trace runs, whichever camera it is for, before the two chains that
        /// hold them. Built here and once: every kernel the trace can ever need is compiled by
        /// `mPass` — 6.3 s on a cold cache, measured — and a frame that stopped for one was a
        /// device reset.
        VisibilityPass mPass;
        CompositePass mComposite;

        /// One bin for everything binned: what differs per scene is the tables, and the camera
        /// arrives with the frame. And one shade, which runs ahead of the bin over the same tables.
        SpriteBinPass mSpriteBin;
        SpriteShadePass mSpriteShade;

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

        /// The world's, which is one of these like any other: what `SceneSlot::world` names. Null
        /// until the first `setScene`.
        std::unique_ptr<DeviceScene> mWorld;

        SceneStats mStats;

        /// Everything between a finished trace and a target, for the frame and for every picture
        /// inside the interface.
        DisplayChain mDisplay;

        /// One sea for everything traced, the doll and the map included: the water is not a
        /// property of a scene, so it is synthesised once a frame here rather than held per scene.
        WavePass mWaves;

        /// What walked through the water, stepped once a frame the world stands in a sea and read
        /// by every picture beside the waves.
        RipplePass mRipples;

        /// One field for everything traced, drawn once for the life of the device. Nothing about
        /// it turns on the weather or the cell — those decide the extinction and the layer's height,
        /// which are numbers the shader already has.
        FogTile mFog;

        /// One pass for everything posed, the doll included: what differs per scene is the
        /// tables, which each `DeviceScene` holds. Before the scenes, which hold it by reference.
        SkinPass mSkinPass;

        /// The hold `RenderProfile::mStressOverlapMs` asked for, or nothing.
        std::unique_ptr<StressPass> mStress;

        /// An empty sprite tiles' list, for a camera that draws no sprites and so binned none.
        /// `VisibilityInputs::mSpriteList` says why one buffer serves every extent.
        Buffer mNoSprites;

        /// What a picture inside the interface sums its census into, which nothing reads: the hit
        /// count and the crossings are the frame's, and a picture traced beside it must not add to
        /// them.
        Buffer mViewCounts;

        /// The interface: `GuiTextures` holds the part with a rule, and the rest is a pipeline, a
        /// scratch vector and a counter with nothing binding them.
        GuiPass mGuiPass;
        GuiTextures mGuiTextures;

        /// The batches, resolved from slots to what the pass wants. Kept so that a frame of GUI
        /// allocates nothing.
        std::vector<GuiDraw> mGuiDraws;

        /// Scenes belonging to pictures rather than to the world, by slot — null until each is
        /// given a scene — and the slots nothing holds.
        std::vector<std::unique_ptr<DeviceScene>> mViewScenes;
        SlotPool mFreeViewScenes;

        /// The picture as bytes, which is what the interface's texture is copied out of. Empty
        /// until something asks for a picture, and grown with `mView`.
        Image mViewTarget;

        /// Null where nothing asked for a window. Last, so it is destroyed first: its command
        /// buffers, out of the device's pool, still hold recordings that blit out of `mTarget`, and destroying
        /// that image while a recording names it is `VUID-vkDestroyImage-image-01000`.
        std::unique_ptr<Presenter> mPresenter;

        /// Raised by `startUpscaler` the first time a mode wants one and null otherwise. It
        /// outlives a mode being turned off, so `upscaling` and not this says whether a frame is
        /// upscaled; `describeDevice` reports from `describeUpscaling`, which asks the device
        /// without standing a runtime up. Which library is behind it, and whether this build has
        /// one, is `makeUpscaler`'s to say.
        std::unique_ptr<Upscaler> mUpscaler;
    };
}
