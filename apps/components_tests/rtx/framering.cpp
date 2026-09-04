#include <cstdint>

#include <gtest/gtest.h>

#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/framering.hpp>
#include <components/rtxvulkan/frameslots.hpp>

#include "harness.hpp"

namespace Rtx
{
    namespace
    {
        struct RtxFrameRingTest : Testing::DeviceTest
        {
            /// Records nothing and submits it, which is a frame as far as the ring is concerned.
            ///
            /// **Empty on purpose.** What is under test is the ring's account of which slot belongs
            /// to whom, and a command buffer with work in it would only make the fences slower.
            void submitEmpty(FrameRing& ring)
            {
                FrameSlot& frame = ring.begin();
                getPool().begin(frame.mCommands);
                ring.submit(frame);
            }
        };

        /// The slot the ring hands out for recording is never one a frame in flight still owns.
        ///
        /// **Two slots and two frames in flight makes them the same slot**, which is the whole of
        /// this: `slotOf(mFrame)` and `slotOf(mFinished)` agree once the ring is full, so the slot a
        /// caller is about to bury into is the oldest frame's. The next drain empties that
        /// graveyard, having waited that frame alone — and the newer frame is still tracing whatever
        /// was in it. `VulkanRenderer::dropTextures` buries a texture the scene let go there, before
        /// anything on its path has drained, and an image went out from under a trace whose
        /// descriptor set still named it.
        ///
        /// The ring is filled first, and `mPending` is what says a frame is still on the queue.
        TEST_F(RtxFrameRingTest, theSlotHandedOutForRecordingIsNotOneAFrameInFlightHolds)
        {
            if (mHarness == nullptr)
                GTEST_SKIP() << "no device";

            const bool countHits = false;
            FrameRing ring(getDevice(), getPool(), countHits);

            // Filled to the brim: nothing collects, so every frame stays in flight, exactly as
            // `RtxTool::runWindow` leaves the ring.
            for (std::uint32_t frame = 0; frame < sFrameSlots; ++frame)
                submitEmpty(ring);

            EXPECT_EQ(ring.getRecording(), sFrameSlots) << "the ring did not take the frames";
            EXPECT_TRUE(ring.slotOf(0).mPending) << "the first frame was finished by something";

            EXPECT_FALSE(ring.recording().mPending) << "the slot handed out is a frame still on the queue";

            // And the same answer to the same question, which is what a caller taking a graveyard
            // and then beginning the frame asks.
            EXPECT_EQ(&ring.recording(), &ring.begin()) << "beginning the frame moved to another slot";

            // Before the ring goes, because its fences and its command buffers go with it and the
            // last frame is still on the queue.
            ring.finishAll();
        }
    }
}
