#include <array>
#include <chrono>
#include <cstdint>
#include <span>

#include <gtest/gtest.h>

#include <vulkan/vulkan_core.h>

#include <components/rtx/texturedata.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/frameslots.hpp>
#include <components/rtxvulkan/handles.hpp>
#include <components/rtxvulkan/texture.hpp>

#include "harness.hpp"
#include "testtexture.hpp"

namespace Rtx
{
    namespace
    {
        struct RtxTextureArrayTest : Testing::DeviceTest
        {
        };

        /// Writing a set's descriptors waits for the submit that bound the set, and for nothing
        /// else.
        ///
        /// **The one per-copy resource that is not a buffer.** A set is bound by handle, so no
        /// address names it, and its bindings allow an update after a bind — but not of a
        /// descriptor a pending trace samples, and a trace samples whichever slots its materials
        /// name. `getSet` is the hand-out and stamps the set the way `Buffer::addressFor` stamps a
        /// buffer; `finishReads` waits for that stamp. Held on the queue while this thread waits,
        /// so the wait cannot return before the hold opens and lasts at least the hold's length.
        TEST_F(RtxTextureArrayTest, syncingASetWaitsForTheSubmitThatBoundIt)
        {
            Device& device = getDevice();
            CommandPool& pool = getPool();

            const SetLayout layout = TextureArray::describeLayout(device);
            Batch setup(pool);
            TextureArray textures(device, setup, layout, 1, {});
            setup.flush();

            // An arrival, owed to every set: what `sync` has to write once the set is free. Ahead
            // of the hold, because a flush behind a held submit waits for the hold.
            const std::array<std::uint8_t, 4> white{ 255, 255, 255, 255 };
            const TextureData arrived = Testing::describeTexel(white, 0);
            Batch arrival(pool);
            textures.write(arrival, std::span(&arrived, 1));
            arrival.flush();

            Testing::HeldSubmit hold(device);
            const VkCommandBuffer binder = pool.allocate(1).front();
            pool.begin(binder);
            EXPECT_NE(textures.getSet(FrameSlot{ 0 }), VK_NULL_HANDLE);
            hold.submit(binder);

            // Long enough that a wait which returned at once is told from one that waited, under three
            // shards of this binary sharing the device.
            constexpr std::chrono::milliseconds held{ 200 };
            const auto asked = std::chrono::steady_clock::now();
            hold.releaseAfter(held);

            textures.finishReads(FrameSlot{ 0 });
            EXPECT_GE(std::chrono::steady_clock::now() - asked, held) << "the sync did not wait for the set's binder";

            // A wait that returned early writes this into a set a submit the hold still keeps on
            // the queue has bound, which is the write the assert fires on.
            textures.sync(FrameSlot{ 0 });

            // And the other set, which nothing bound, waits for nothing.
            const auto other = std::chrono::steady_clock::now();
            textures.finishReads(FrameSlot{ 1 });
            EXPECT_LT(std::chrono::steady_clock::now() - other, held);
            textures.sync(FrameSlot{ 1 });
        }
    }
}
