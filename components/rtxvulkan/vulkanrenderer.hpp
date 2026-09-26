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
#include <components/rtx/kernelprogress.hpp>
#include <components/rtx/memoryreport.hpp>
#include <components/rtx/reconstruction.hpp>
#include <components/rtx/refusal.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/shaders/visibility.h>
#include <components/rtx/slot.hpp>
#include <components/rtx/texturedata.hpp>
#include <components/rtx/upscale.hpp>
#include <components/rtx/wavespectrum.hpp>
#include <components/sdlutil/vsyncmode.hpp>

#include "accumulatepass.hpp"
#include "atrouspass.hpp"
#include "commands.hpp"
#include "compositepass.hpp"
#include "device.hpp"
#include "digestpass.hpp"
#include "displaychain.hpp"
#include "framering.hpp"
#include "groundcompositepass.hpp"
#include "guidrawer.hpp"
#include "handles.hpp"
#include "image.hpp"
#include "instance.hpp"
#include "mipchainpass.hpp"
#include "picturetracer.hpp"
#include "presenttargets.hpp"
#include "sceneslots.hpp"
#include "shadingpass.hpp"
#include "skinpass.hpp"
#include "spritelightpass.hpp"
#include "spritepasses.hpp"
#include "stresspass.hpp"
#include "texture.hpp"
#include "tracechain.hpp"
#include "tracemedia.hpp"
#include "visibilitypass.hpp"

namespace Rtx
{
    class Presenter;
    class Upscaler;

    /// `Renderer` over Vulkan.
    class VulkanRenderer final : public Renderer
    {
    public:
        /// Throws `Unsupported` where this machine cannot run it and `DeviceError` where the device
        /// failed. `createVulkanRenderer` is how a host makes one.
        explicit VulkanRenderer(const RendererOptions& options);
        ~VulkanRenderer() override;

        std::string describeDevice() const override;
        bool isValidating() const override;
        void resetHistory() override;

        void setScene(SceneSlot slot, const SceneDesc& scene, std::span<const TextureData> textures) override;
        void extendScene(SceneSlot slot, const SceneDesc& scene, std::span<const TextureData> arrived) override;
        SceneHeld describeHeld(SceneSlot slot) const override;
        std::span<const Refusal> getRefusals(SceneSlot slot) const override;
        void dropTextures(SceneSlot slot, std::span<const Index> textures) override;
        void placeScene(SceneSlot slot, const SceneDesc& scene) override;
        const SceneStats& getSceneStats() const override { return mStats; }
        MemoryReport getMemoryReport() const override;
        void resize(std::uint32_t width, std::uint32_t height) override;
        void setUpscale(Upscale upscale) override;

        void setVerticalSync(SDLUtil::VSyncMode mode) override;

        /// The driver's pacing, which is the presenter's and nothing without one — a headless
        /// renderer is paced by whoever calls it.
        bool pacesFrames() const override;
        void setPacing(const Pacing& pacing) override;
        void skipFrame() override;
        void awaitFrame() override;
        void endSimulation(bool flash) override;
        std::optional<LatencyReport> describeLatency() const override;
        FrameExtents getExtents() const override;
        const RenderProfile& getProfile() const override { return mProfile; }
        KernelProgress awaitKernels(std::chrono::milliseconds patience) override;
        Reconstruction renderFrame(const Shaders::VisibilityConstants& camera, const FrameOptions& options) override;
        std::uint64_t getFrameCount() const override;
        std::optional<FrameResult> finishFrame() override;
        std::optional<FrameResult> collectFrame() override;
        void presentFrame() override;

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

        /// What a test asks of this backend and a game never does. None is on a frame path: each
        /// that reads submits a copy and waits for it, so none of those is const.

        /// The device the renderer made, for a test that holds its memory to a budget of its own.
        const Device& getDevice() const { return mDevice; }

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
        /// The passes both chains trace with, which every member it names is declared ahead of
        /// the chains to be built for.
        TracePasses describeTracePasses() const;

        /// The image this frame writes, with the present that last read it waited for — once per
        /// frame, at the first of the trace and the interface to want it.
        Image& claimTarget();

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

        /// Whether a frame is upscaled, which is the mode alone: a mode that wants a runtime has
        /// one, because `setUpscale` raises it before it moves the mode. The runtime outlives a
        /// mode being turned off, because raising it again costs a quarter of a second.
        bool upscaling() const { return mProfile.mUpscaling.mMode != Upscale::Off; }

        // Declaration order is destruction order reversed, and everything below the device is built
        // on it.
        Instance mInstance;

        Device mDevice;

        /// Whether the frame this builds counts for the host. `RendererOptions::mCounting` says why
        /// the game's does not.
        bool mCounting = false;

        /// What the run decided once, read where each knob is used: how wide both chains store
        /// their radiance, how long the queue is held, and what the frames are traced under —
        /// `mUpscaling` as it was handed over, and then whatever `setUpscale` moved it to, which
        /// rebuilds every target and is why the mode is a setting rather than a frame option. What
        /// a frame carries — the reconstruction request, the exposure, the delight, the sample —
        /// is read off the frame's own blocks instead, which is where a frame that asks otherwise
        /// says so.
        RenderProfile mProfile;

        /// Whether a frame's counts come back to the host at all: where the trace counts its
        /// hits, and where a hold leaves its reading. One answer, read where the block is cleared,
        /// ordered for the host and read back, so the three cannot disagree.
        bool mReadsCounts = false;

        /// The frames in flight and what each came to. After the flag it is handed.
        FrameRing mRing{ mDevice, mReadsCounts };

        /// Whether the upscaler's history is worthless. Set by `resetHistory` and spent by the next
        /// frame that upscales, which is not always the one after. The other histories keep their
        /// own: the chain's, `TraceChain::resetHistory`, and the display's.
        bool mUpscalerStale = false;

        /// When the last frame was recorded, so the next can say how long ago that was. Measured
        /// here rather than asked of the caller, because this is the function the frames the
        /// upscaler reconstructs across pass through.
        std::optional<std::chrono::steady_clock::time_point> mLastFrameAt;

        /// The frame as bytes at the output extent, which is what anything outside this reads: two
        /// images, swapped by every present, and the one the last present read. It is also where
        /// that extent is stated — `PresentTargets::getExtent` — rather than beside it in a pair
        /// of members something would have to keep level. `PresentTargets`
        /// says why there are two.
        PresentTargets mTargets;

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
        /// `mPass`, on threads of its own — ten seconds on a cold cache, measured — and waited for
        /// ahead of every trace, because a frame that stopped for one was a device reset.
        VisibilityPass mPass;
        CompositePass mComposite;

        /// One bin for everything binned: what differs per scene is the tables, and the camera
        /// arrives with the frame. And one shade, which runs ahead of the bin over the same tables.
        SpriteBinPass mSpriteBin;
        SpriteShadePass mSpriteShade;

        /// The denoiser's two passes, one pipeline each for both chains: each chain keeps the
        /// history and the scratch its own camera needs.
        AccumulatePass mAccumulate;
        AtrousPass mFilter;

        /// What the frame is traced into, at the render extent — which is the output extent
        /// wherever nothing upscales.
        TraceChain mFrame;

        /// The camera the last frame was traced with, for reprojecting this one against. Its basis
        /// is all zero until a frame is traced, and after a resize or a new scene, which the shader
        /// reads as "there is no previous frame" and answers with no motion at all.
        Shaders::VisibilityConstants mPreviousCamera{};

        SceneStats mStats;

        /// Everything between a finished trace and a target, for the frame and for every picture
        /// inside the interface.
        DisplayChain mDisplay;

        /// What folds the frame's images into `FrameResult::mDigest`, on the frames that ask.
        DigestPass mDigest;

        TraceMedia mMedia;

        /// One pass for everything posed, the doll included: what differs per scene is the
        /// tables, which each `DeviceScene` holds. Before the scenes, which hold it by reference.
        SkinPass mSkinPass;

        /// One pass for every texture's missing chain, one for every texture's shading map and
        /// one for every sprite's light bake, the doll's and the maps' included, for the same
        /// reason and held the same way; the bundle the arrays are handed, after the three it
        /// names.
        MipChainPass mMipChainPass;
        ShadingPass mShadingPass;
        SpriteLightPass mSpriteLightPass;
        TexturePasses mTexturePasses;

        /// One pass for every chunk's flattened ground, for the same reason. After the texture
        /// layout, whose set it binds.
        GroundCompositePass mGroundPass;

        /// The hold `RenderProfile::mStressOverlapMs` asked for, or nothing.
        std::unique_ptr<StressPass> mStress;

        /// After the passes above, which every scene holds by reference.
        SceneSlots mScenes;

        GuiDrawer mGui;

        /// After the media, the display and the interface, which it holds by reference.
        PictureTracer mPictures;

        /// Null where nothing asked for a window. After `mTargets`, so it is destroyed before them:
        /// its command buffers, out of the device's pool, still hold recordings that blit out of
        /// their images, and destroying an image while a recording names it is
        /// `VUID-vkDestroyImage-image-01000`.
        std::unique_ptr<Presenter> mPresenter;

        /// Raised by `startUpscaler` the first time a mode wants one and null otherwise. It
        /// outlives a mode being turned off, so `upscaling` and not this says whether a frame is
        /// upscaled; `describeDevice` reports from `describeUpscaling`, which asks the device
        /// without standing a runtime up. Which library is behind it, and whether this build has
        /// one, is `makeUpscaler`'s to say.
        std::unique_ptr<Upscaler> mUpscaler;
    };
}
