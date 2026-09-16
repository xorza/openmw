#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include <vulkan/vulkan_core.h>

#include <components/rtx/reconstruction.hpp>
#include <components/rtx/shaders/camera.h>

#include "accumulatepass.hpp"
#include "atrouspass.hpp"
#include "fogvolume.hpp"
#include "frameslots.hpp"
#include "gbuffer.hpp"
#include "handles.hpp"
#include "image.hpp"
#include "spritebin.hpp"

namespace Rtx
{
    class CompositePass;
    class Device;
    class GpuTimer;
    class SpriteBinPass;
    class SpriteShadePass;
    class VisibilityPass;
    struct TraceRecording;

    /// Everything one camera's trace writes, at one extent — one chain however many cameras have
    /// one, so a barrier cannot go missing from a second copy. What differs between two of these
    /// is what the caller hands in: the extent, what may be done with the composite's image
    /// afterwards, and whether the chain is sized exactly or grown to fit. What becomes of a
    /// finished picture — the frame's presented pair, a picture's byte target — is not here.
    class TraceChain
    {
    public:
        /// The denoising passes are built here and the images are not: nothing has an extent
        /// until `resize` or `grow` is called.
        ///
        /// @param channels, fog the layouts every `GBuffer` and every `FogVolume` here is shaped by.
        /// @param visibility, composite, spriteBin, spriteShade the passes every trace runs,
        ///        whichever camera it is for: the renderer keeps one of each, and what differs
        ///        between two chains is the extent and what becomes of the picture.
        /// @param colourUsage what the composite's output has done to it besides being written: an
        ///        upscaler samples a frame's and a measurement copies it out.
        /// @param colourName what a capture and a validation message call that image.
        TraceChain(const Device& device, const SetLayout& channels, const SetLayout& fog,
            const VisibilityPass& visibility, const CompositePass& composite, const SpriteBinPass& spriteBin,
            const SpriteShadePass& spriteShade, const std::filesystem::path& shaders, VkImageUsageFlags colourUsage,
            std::string_view colourName);

        /// Builds the chain at exactly this extent, whatever it was before. The caller has waited
        /// for anything still reading what this replaces.
        ///
        /// @param radiance how wide the two radiance channels and the frame composed from them are
        ///        stored — the run's choice, which `Rtx::RadianceWidth` argues.
        void resize(std::uint32_t width, std::uint32_t height, RadianceWidth radiance);

        /// Makes the chain at least this big, keeping whatever extent it already reached on either
        /// axis. Nothing where it already `holds` the size. Grown and never shrunk, because a
        /// smaller picture uses a corner of a larger one's images rather than rebuilding them.
        void grow(std::uint32_t width, std::uint32_t height, RadianceWidth radiance);

        /// The extent the images are at, which is what a dispatch over the whole of one covers.
        /// Nought until the first `resize` or `grow`.
        std::uint32_t getWidth() const { return mWidth; }
        std::uint32_t getHeight() const { return mHeight; }

        /// Whether the images exist, which is the same question as whether the extent is set.
        bool isBuilt() const { return !mColour.isEmpty(); }

        /// Whether a picture this big fits what is built, which is what `grow` would leave alone.
        bool holds(std::uint32_t width, std::uint32_t height) const
        {
            return isBuilt() && width <= mWidth && height <= mHeight;
        }

        /// The composite's output: one picture in linear radiance, before anything upscales it and
        /// before the display curve.
        const Image& getColour() const { return mColour; }

        /// What the trace writes and the composite reads: one picture's light, still in pieces.
        const GBuffer& getChannels() const { return *mChannels; }

        /// Where the air is integrated, one column to a block of pixels.
        const FogVolume& getFogVolume() const { return *mFogVolume; }

        /// Records one camera's whole trace, from the discards it opens with to the barrier after
        /// the composite, and hands back the composite's output. What the caller keeps is what a
        /// frame has and a picture has not — the frame ring, the upscaler, the lens, the measured
        /// exposure and the display curve.
        const Image& record(VkCommandBuffer commands, const TraceRecording& what);

    private:
        /// The bounce resolved: the temporal mean, and then the cascade over it, with the barrier
        /// between them that makes this one call.
        ///
        /// @param timer null where the run is not being timed, which a picture is not.
        const Image& recordDenoise(
            VkCommandBuffer commands, const Shaders::Camera& camera, float far, bool historyLost, GpuTimer* timer);

        const Device& mDevice;

        const SetLayout& mChannelLayout;
        const SetLayout& mFogVolumeLayout;

        const VisibilityPass& mVisibility;
        const CompositePass& mComposite;
        const SpriteBinPass& mSpriteBin;
        const SpriteShadePass& mSpriteShade;

        VkImageUsageFlags mColourUsage;
        std::string mColourName;

        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;

        Image mColour;
        std::unique_ptr<GBuffer> mChannels;
        std::unique_ptr<FogVolume> mFogVolume;

        /// One sprite bin per frame in flight — `TraceRecording::mTraceSlot` picks — so the frame
        /// behind keeps the tables its trace reads while this frame's bin writes its own.
        PerSlot<SpriteBin> mBins;

        /// Held by value rather than built with the extent, because what they read is pushed at
        /// record time. The filter is not const only because it keeps a channel the size of the
        /// picture.
        AccumulatePass mAccumulate;
        AtrousPass mFilter;
    };
}
