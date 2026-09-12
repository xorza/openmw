#pragma once

#include <cstdint>
#include <memory>

#include <vulkan/vulkan_core.h>

#include "image.hpp"

namespace Rtx
{
    class CommandPool;
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
        /// `growViewTargets` makes an image the same pass draws over.
        static constexpr VkFormat sFormat = VK_FORMAT_R8G8B8A8_UNORM;

        /// Makes both, black and in `VK_IMAGE_LAYOUT_GENERAL`, because the GUI is drawn over one
        /// whether or not a frame was traced into it.
        void resize(const Device& device, CommandPool& pool, std::uint32_t width, std::uint32_t height);

        bool isOpen() const { return mTarget != nullptr; }

        /// The one this frame writes.
        Image& current() { return *mTarget; }
        const Image& current() const { return *mTarget; }

        /// Says the current one has just been presented, and hands the next frame the other one.
        ///
        /// @param wait called with the image the next frame will write, once it is the current one.
        ///        Here rather than at the first write, because two presents went by since that
        ///        image was last read, so the wait returns at once.
        template <class Wait>
        void presented(Wait&& wait)
        {
            mPresented = mTarget.get();
            mTarget.swap(mSpare);

            wait(*mTarget);
        }

        /// The one the last present read, or null where nothing has been presented at all.
        ///
        /// **Named apart because that null is the whole question `readPixels` asks**, and a headless
        /// run never answers it.
        const Image* lastPresented() const { return mPresented; }

    private:
        /// Numbered rather than named: which one is being written changes every present, so a name
        /// that said so would be wrong on half the frames it appeared in.
        std::unique_ptr<Image> mTarget;
        std::unique_ptr<Image> mSpare;

        const Image* mPresented = nullptr;
    };
}
