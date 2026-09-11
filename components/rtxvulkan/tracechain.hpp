#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/camera.h>

#include "accumulatepass.hpp"
#include "atrouspass.hpp"
#include "fogvolume.hpp"
#include "gbuffer.hpp"
#include "image.hpp"

namespace Rtx
{
    class CommandPool;
    class Device;
    class GpuTimer;
    class SetLayout;
    struct TraceRecording;

    /// Everything one camera's trace writes, at one extent.
    ///
    /// **One chain, however many cameras have one.** A colour image, a `GBuffer`, a `FogVolume`, an
    /// accumulator and a filter are sized together and recorded together, so a second camera that
    /// spelled them out again would be a second place for a barrier to go missing from. `record` is
    /// the other half of that: the sequence is written here rather than once per camera.
    ///
    /// What differs between two of these is what the caller hands in: the extent, what may be done
    /// with the composite's image afterwards, and whether the chain is sized exactly or grown to
    /// fit.
    ///
    /// **Neither the frame's presented pair nor a picture's byte target is here.** Those are what
    /// becomes of a finished picture rather than what a trace writes into, and the two differ in
    /// kind: a present reads its image across a submit, so a frame needs two of them and a picture
    /// needs one.
    class TraceChain
    {
    public:
        /// The passes are built here and the images are not: nothing has an extent until `resize`
        /// or `grow` is called.
        ///
        /// @param channels, fog the layouts every `GBuffer` and every `FogVolume` here is shaped
        ///        by, which outlive this because the trace's pipeline names them when it is built.
        /// @param colourUsage what the composite's output has done to it besides being written. An
        ///        upscaler samples a frame's and a measurement copies it out, and nothing but the
        ///        tone curve reads a picture's.
        /// @param colourName what a capture and a validation message call that image, which is how
        ///        two chains are told apart in both.
        TraceChain(const Device& device, CommandPool& pool, const SetLayout& channels, const SetLayout& fog,
            const std::filesystem::path& shaders, VkImageUsageFlags colourUsage, std::string_view colourName);

        TraceChain(const TraceChain&) = delete;
        TraceChain& operator=(const TraceChain&) = delete;

        /// Builds the chain at exactly this extent, whatever it was before.
        ///
        /// The caller is expected to have waited for anything still reading what this replaces.
        void resize(std::uint32_t width, std::uint32_t height);

        /// Makes the chain at least this big, keeping whatever extent it already reached on either
        /// axis, and answers whether anything was built.
        ///
        /// **Grown and never shrunk**, because there are three or four picture sizes in the whole
        /// game and every pass takes the extent it is dispatched over: a smaller picture uses a
        /// corner of a larger one's images rather than rebuilding them. Each axis goes to the
        /// larger of what was there and what is wanted, so a wide picture after a tall one does not
        /// throw the tall one's height away and build it again next time.
        bool grow(std::uint32_t width, std::uint32_t height);

        /// The extent the images are at, which is what a dispatch over the whole of one covers.
        /// Nought until the first `resize` or `grow`.
        std::uint32_t getWidth() const { return mWidth; }
        std::uint32_t getHeight() const { return mHeight; }

        /// Whether the images exist, which is the same question as whether the extent is set.
        bool isBuilt() const { return mColour != nullptr; }

        /// The composite's output: one picture in linear radiance, before anything upscales it and
        /// before the display curve.
        const Image& getColour() const { return *mColour; }

        /// What the trace writes and the composite reads: one picture's light, still in pieces.
        const GBuffer& getChannels() const { return *mChannels; }

        /// Where the air is integrated, one column to a block of pixels.
        const FogVolume& getFogVolume() const { return *mFogVolume; }

        /// What the accumulator blended, which is a channel a measurement can be read out of. It
        /// asserts where nothing denoised, so a caller asks `Rtx::hasFrameImage` before it comes to
        /// that.
        const Image& getBlended() const { return mAccumulate.getBlended(); }

        /// Records one camera's whole trace, from the discards it opens with to the barrier after
        /// the composite.
        ///
        /// **One statement of the chain, because there are two cameras and one chain.** The frame
        /// and the pictures inside the interface each spelled the sequence out, and a barrier is
        /// exactly the kind of step that goes missing from the second copy.
        ///
        /// **What the caller keeps is what a frame has and a picture has not** — the frame ring, the
        /// upscaler, the lens, the measured exposure and the display curve. `TraceRecording` is
        /// everything the sequence itself needs, and every field of it is per-call.
        ///
        /// @return the composite's output, which is `getColour()` — answered so that a caller reads
        ///         what this wrote rather than reaching for the image and hoping it is the one.
        const Image& record(VkCommandBuffer commands, const TraceRecording& what);

    private:
        /// The bounce resolved: the temporal mean, and then the cascade over it.
        ///
        /// **The barrier between them is the reason this is one call.** Two compute dispatches are
        /// unordered inside a command buffer, so the cascade reads what the accumulator wrote only
        /// where something says so.
        ///
        /// @param timer null where the run is not being timed, which a picture is not.
        const Image& recordDenoise(
            VkCommandBuffer commands, const Shaders::Camera& camera, float far, bool historyLost, GpuTimer* timer);

        const Device& mDevice;
        CommandPool& mPool;

        const SetLayout& mChannelLayout;
        const SetLayout& mFogVolumeLayout;

        VkImageUsageFlags mColourUsage;
        std::string mColourName;

        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;

        std::unique_ptr<Image> mColour;
        std::unique_ptr<GBuffer> mChannels;
        std::unique_ptr<FogVolume> mFogVolume;

        /// Held by value rather than built with the extent, because what they read is pushed at
        /// record time. The filter is not const only because it keeps a channel the size of the
        /// picture.
        AccumulatePass mAccumulate;
        AtrousPass mFilter;
    };
}
