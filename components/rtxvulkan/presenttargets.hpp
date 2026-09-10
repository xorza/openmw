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
    ///
    /// **Two images, swapped by every present, and the one the last present read.** A present's blit
    /// reads its image long after the call that queued it returned — it waits the acquire semaphore,
    /// which under FIFO the presentation engine signals when it lets that swapchain image go — and
    /// the discard a frame opens with is sourced at `TOP_OF_PIPE`, which waits for nothing. One image
    /// would have each frame rewriting what the last is still being read out of, and no barrier can
    /// order that: a source scope does not reach across a submit.
    ///
    /// **One type, because that rule is what ties the three together.** They were three members and
    /// an extent beside them, and the swap, the clear and the wait that make them safe were written
    /// at three places with nothing naming the invariant they keep.
    class PresentTargets
    {
    public:
        /// What the finished picture is encoded into, and so what the GUI pass is compiled against.
        ///
        /// **Four bytes a pixel and not display-encoded by the hardware**: the tone curve has
        /// already run by the time anything is written here.
        ///
        /// **Here, because a picture inside the interface takes it too.** `growViewTargets` makes an
        /// image the same pass draws over, so a second spelling of the format would be a pipeline
        /// compiled against one and handed the other.
        static constexpr VkFormat sFormat = VK_FORMAT_R8G8B8A8_UNORM;

        /// Makes both, black and in `VK_IMAGE_LAYOUT_GENERAL`, replacing whatever was there.
        ///
        /// **Black from the moment they exist.** Everything that reads a target expects that layout,
        /// and the GUI is drawn over one whether or not a frame has been traced into it — a main
        /// menu and a loading screen have no world behind them.
        void resize(const Device& device, CommandPool& pool, std::uint32_t width, std::uint32_t height);

        bool isOpen() const { return mTarget != nullptr; }

        /// The one this frame writes.
        Image& current() { return *mTarget; }
        const Image& current() const { return *mTarget; }

        /// Says the current one has just been presented, and hands the next frame the other one.
        ///
        /// @param wait called with the image the next frame will write, once it is the current one.
        ///        **Here rather than at the first write, which is what makes it free.** Two presents
        ///        have gone by since that image was last read, so its fence is signalled and the
        ///        wait returns at once; asking at the first write instead would put a frame face to
        ///        face with the present before it, and that one can still be waiting on the
        ///        presentation engine.
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
