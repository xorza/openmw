#pragma once

#include <cstdint>
#include <utility>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/tone.h>

#include "formats.hpp"
#include "image.hpp"

namespace Rtx
{
    class Device;

    /// The frame as bytes at the output extent, which is what anything outside the renderer reads.
    /// Two images swapped by every present, because a present's blit reads its image long after
    /// the call returned — it waits the acquire semaphore — and the discard a frame opens with
    /// waits for nothing, so one image would be rewritten while still read, and no barrier reaches
    /// across a submit.
    class PresentTargets
    {
    public:
        /// What the finished picture is encoded into, and so what the GUI pass is compiled against.
        /// Not display-encoded by the hardware, because the tone curve ran already. Here, because
        /// `PictureTracer::grow` makes an image the same pass draws over.
        static constexpr VkFormat sFormat = toVulkanFormat(TONE_TARGET_FORMAT);

        /// Makes both, black and in `VK_IMAGE_LAYOUT_GENERAL`, because the GUI is drawn over one
        /// whether or not a frame was traced into it.
        void resize(const Device& device, std::uint32_t width, std::uint32_t height);

        bool isOpen() const { return !mTarget.isEmpty(); }

        /// What both images are, and so what every frame is presented at — the one statement of
        /// the output extent, because these are the images that carry it. Zero before the first
        /// `resize`, which is what an asked extent is compared against.
        VkExtent2D getExtent() const { return VkExtent2D{ mTarget.getWidth(), mTarget.getHeight() }; }

        /// The one this frame writes, made writable: `wait` is called with it on the first claim
        /// after a present and not again. At the first write rather than at the present, because
        /// with two frames in flight the blit that last read this image is behind a trace still
        /// running when the present swaps, and waiting there is waiting that trace out — the gap
        /// `collectFrame` closes, opened again at the other end of the frame. By the first write
        /// the ring has waited the frame before last out, and that blit was queued right behind it.
        template <class Wait>
        Image& claim(Wait&& wait)
        {
            if (!mClaimed)
            {
                wait(mTarget);
                mClaimed = true;
            }

            return mTarget;
        }

        /// The one this frame writes, for a record after the claim and for a read.
        Image& current() { return mTarget; }
        const Image& current() const { return mTarget; }

        /// Says the current one has just been presented, and hands the next frame the other one:
        /// the two swap places, so what was presented is the spare from here.
        void presented()
        {
            std::swap(mTarget, mSpare);
            mSparePresented = true;
            mClaimed = false;
        }

        /// The one the last present read, or null where nothing was presented at all — named apart
        /// because that null is the whole question `readPixels` asks, and a headless run never
        /// answers it.
        const Image* lastPresented() const { return mSparePresented ? &mSpare : nullptr; }

    private:
        /// Numbered rather than named: which one is being written changes every present, so a name
        /// that said so would be wrong on half the frames it appeared in.
        Image mTarget;
        Image mSpare;

        /// Whether any present has happened since `resize`, which is whether the spare is what a
        /// present last read.
        bool mSparePresented = false;

        /// Whether the current one's last reader has been waited for since it became current.
        bool mClaimed = false;
    };
}
