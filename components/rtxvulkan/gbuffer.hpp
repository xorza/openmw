#pragma once

#include <array>
#include <cstdint>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/gbuffer.h>

#include "image.hpp"
#include "setlayout.hpp"

namespace Rtx
{
    class Device;

    /// What the trace leaves behind, before anything has decided what the picture looks like.
    ///
    /// **A picture cannot be filtered and these can.** One bounce per pixel is noisy, and the only
    /// thing that removes noise without removing detail is a blur that runs over the light alone —
    /// which means the light has to still be separate from the surface it landed on when the blur
    /// reaches it. By the time a pixel is a colour, the albedo has been multiplied in, the fog has
    /// been laid over it and the curve has been applied; there is nothing left to filter that would
    /// not also smear the wall's texture.
    ///
    /// So the trace writes what it knows in the form the next pass can use, and the composite puts
    /// it back together:
    ///
    ///     colour = direct + albedo * filter(indirect * transmittance)
    ///
    /// **And this is the same buffer Ray Reconstruction reads.** It asks for exactly this —
    /// demodulated radiance, the albedo to put back, normals and depth — so the split earns its
    /// place twice over even if the filter written on top of it is later replaced.
    class GBuffer
    {
    public:
        /// How many channels there are, which is also how many bindings the set declares.
        /// `shaders/gbuffer.h` is where each one's number is said, for this and for the trace that
        /// writes them.
        static constexpr std::uint32_t sChannels = Shaders::CHANNEL_COUNT;

        GBuffer(const Device& device, const SetLayout& layout, std::uint32_t width, std::uint32_t height);

        /// The set every `GBuffer` is addressed through, made once and outliving all of them.
        ///
        /// **Separate from the buffer because a pipeline layout names every set it will ever be
        /// handed**, and the trace's pipelines are built before any camera has a size.
        static SetLayout describeLayout(const Device& device);
        ~GBuffer();

        GBuffer(const GBuffer&) = delete;
        GBuffer& operator=(const GBuffer&) = delete;

        /// Direct light, emission, the sky and water, with the fog already over all of it.
        const Image& getDirect() const { return mDirect; }

        /// One bounce with the albedo divided out, times whatever the water and the air took off it
        /// on the way to the eye. The only channel a filter may touch.
        const Image& getIndirect() const { return mIndirect; }

        /// The surface's own diffuse albedo, with nothing of the path in it.
        ///
        /// **Two questions were being answered by one number.** The composite wants the albedo times
        /// what the path took, so that multiplying the bounce by it puts both back at once; Ray
        /// Reconstruction wants the albedo alone, because it is dividing the light by it. Folding
        /// the transmittance in here answered the first and quietly failed the second, and the
        /// upscaler demodulated by a foggy albedo everywhere there was weather. The transmittance
        /// now rides with the light it attenuated, on `getIndirect`.
        const Image& getAlbedo() const { return mAlbedo; }

        /// The surface's specular albedo — its reflectance at the angle it was seen from.
        ///
        /// **Zero over every solid surface, and that is the shading model speaking.** Nothing here
        /// answers a ray with a specular lobe except the water, so nothing else has a specular half
        /// for an upscaler to separate out. This used to be a full-precision image cleared to zero
        /// once and sampled every frame ever after, which is a different thing: a placeholder for an
        /// answer rather than the answer.
        const Image& getSpecular() const { return mSpecular; }

        /// The shading normal in `xyz` and the surface's roughness in `w`.
        ///
        /// **The layout Ray Reconstruction reads when it is told roughness is packed**, which is one
        /// resource fewer to write and to bind than handing it a separate image. The distance a
        /// filter compares edges by used to live in `w` and is now the depth channel's second
        /// component, because two different questions were being answered by one number.
        ///
        /// Both halves are what the shading actually used: the wave's normal over water rather than
        /// the plane's, and one over anything Lambert rather than one over everything.
        const Image& getGuide() const { return mGuide; }

        /// Where each surface stood on the previous frame's screen, less where it stands on this
        /// one, in pixels. Zero where nothing was hit or where the surface was behind the old eye.
        ///
        /// **Full floats, and not because the numbers are large.** A motion vector spans the frame
        /// when the camera turns — a couple of thousand pixels — and a half float lands only on
        /// whole pixels above 1024, which is the sub-pixel accuracy an upscaler reconstructs from
        /// thrown away exactly where the camera is moving fastest.
        const Image& getMotion() const { return mMotion; }

        /// Clip depth in `r` and the distance from the eye in `g`.
        ///
        /// **Two answers because they are two questions.** The first is what a rasterizer with this
        /// frustum would have written — zero at the near plane, one at the far one, hyperbolic
        /// between — and exists so an upscaler's disocclusion test is looking at the depth it
        /// expects. The second is what the filter compares surfaces by, in world units, because a
        /// tolerance measured against a clip value would mean something different at every distance:
        /// most of that range is spent within a few units of the eye.
        const Image& getDepth() const { return mDepth; }

        /// Where what the water reflects stood on the previous frame's screen, in pixels.
        ///
        /// **Water is shaded where it is seen and shows what is somewhere else.** The motion channel
        /// describes the surface, so a reflection reprojects with the water carrying it rather than
        /// with the thing reflected — a shoreline mirrored in a lake swims as the camera walks. NGX
        /// takes this beside the ordinary field as "motion vectors of reflected objects like for
        /// mirrored surfaces", and weighs the two by the specular albedo it already has.
        const Image& getReflectionMotion() const { return mReflectionMotion; }

        /// Where a sprite reached this frame, as one or nought.
        ///
        /// **A sprite is the one thing in the frame with no motion vector of its own.** The trace
        /// writes one per pixel from the surface a primary ray hit, so rain, smoke and every other
        /// emitter is reprojected with whatever wall stands behind it. NGX's own word for the
        /// remedy is a mask "to identify which pixels contains particles, essentially that are not
        /// drawn as part of base pass", and the frame already knows: `spritesAlong` returns what the
        /// sprites left of the light behind them.
        const Image& getParticleMask() const { return mParticleMask; }

        /// Where the reconstruction must not carry the past forward, from nought to one.
        ///
        /// The sprites above, and the water with them: water is shaded on the primary hit, so what
        /// is reflected in it moves with the surface rather than with itself, and a history
        /// accumulated over that is a history of the wrong thing.
        const Image& getBiasMask() const { return mBiasMask; }

        /// What the star field has to be drawn through at each pixel, per channel.
        ///
        /// **The one channel written for a pass rather than for a filter.** The field is drawn after
        /// the upscaler, at the resolution the frame is shown at, because a point source is what a
        /// temporal upscaler removes — and a pass that late has no moons, no cloud deck, no window
        /// pane, no water and no fog to draw the field behind. This is all of them multiplied
        /// together: the sky's own order says what it left of the field, and the path says what it
        /// took off everything. Nought wherever a ray hit something, which is also how that pass
        /// knows there is no sky here at all.
        const Image& getStarsShown() const { return mStarsShown; }

        /// The layer the eye sees through, its coverage and its own motion — what Ray Reconstruction
        /// takes as `pInTransparencyLayer` and its two companions. `gbuffer.h` says why they are
        /// three channels and not one composite.
        ///
        /// **The opacity is three channels for one number**, because the upscaler reads it as a
        /// colour: `GBuffer::sLayer` says what a single channel drew instead.
        ///
        /// **The motion is full floats for `getMotion`'s reason and not for this channel's own.** A
        /// raindrop's vector spans the frame when the camera turns, and a half float lands only on
        /// whole pixels above 1024 — which throws away the sub-pixel accuracy the upscaler is being
        /// handed the layer for. The colour beside it is halved, and `GBuffer::sLayer` says why the
        /// two answers differ.
        const Image& getTransparency() const { return mTransparency; }
        const Image& getTransparencyOpacity() const { return mTransparencyOpacity; }
        const Image& getTransparencyMotion() const { return mTransparencyMotion; }

        /// The set that names every channel, for the pass that writes them.
        ///
        /// **Written once, when the channels are made, and never again.** The images live as long as
        /// this does and a resize builds a new one of both, so nothing here is rewritten while a
        /// submitted frame is still reading it — which is the whole reason a set is affordable where
        /// a push was not.
        VkDescriptorSet getSet() const { return mSet; }

        std::uint32_t getWidth() const { return mDirect.getWidth(); }
        std::uint32_t getHeight() const { return mDirect.getHeight(); }

        /// Discards the contents and makes every channel writable, which is how a frame starts.
        ///
        /// Waits for the previous frame's composite to have read them, so that one set of channels
        /// can serve a window that keeps several frames in flight.
        void begin(VkCommandBuffer commands) const;

        /// Orders the pass that wrote them against the pass about to read them, or in the
        /// accumulator's case to write one of them back.
        void handOver(VkCommandBuffer commands) const;

    private:
        /// Every channel this holds, in binding order, so the barrier sweeps and the set cannot
        /// name different ones.
        ///
        /// **They did.** The list was written out twice and a channel added to one of them was left
        /// out of the other, which is a frame reading an image nothing had transitioned — reported
        /// by the layers as a layout the descriptor did not expect, and by nothing at all in a build
        /// without them.
        std::array<const Image*, sChannels> everyChannel() const;

        /// Frees the pool, from either end: the destructor, and the constructor's own unwind.
        ///
        /// **A constructor whose body throws gets no destructor**, so a pool made in one and left
        /// there is a pool nothing owns. `ComputePipeline` is the same shape for the same reason,
        /// and this is the second hand-written unwind in the renderer rather than the first.
        void destroy();

        const Device& mDevice;

        Image mDirect;
        Image mIndirect;
        Image mAlbedo;
        Image mSpecular;
        Image mGuide;
        Image mMotion;
        Image mDepth;
        Image mReflectionMotion;
        Image mParticleMask;
        Image mBiasMask;
        Image mStarsShown;
        Image mTransparency;
        Image mTransparencyOpacity;
        Image mTransparencyMotion;

        VkDescriptorPool mPool = VK_NULL_HANDLE;
        VkDescriptorSet mSet = VK_NULL_HANDLE;
    };
}
