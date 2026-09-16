#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>

#include <gtest/gtest.h>

#include <vulkan/vulkan_core.h>

#include <components/rtxvulkan/buffer.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/framering.hpp>
#include <components/rtxvulkan/frameslots.hpp>
#include <components/rtxvulkan/graveyard.hpp>

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
                FrameRecord& frame = ring.begin();
                getPool().begin(frame.mWorld.mCommands);
                ring.submit(frame);
            }
        };

        /// The slot the ring hands out for recording is never one a frame in flight still owns.
        ///
        /// **Two slots and two frames in flight makes them the same slot**, which is the whole of
        /// this: `slotOf(mFrame)` and `slotOf(mFinished)` agree once the ring is full, so the slot a
        /// caller is about to record into is the oldest frame's, and the next drain waits that
        /// frame alone while the newer one is still tracing.
        ///
        /// The ring is filled first, and `FrameState::Submitted` is what says a frame is still on
        /// the queue.
        TEST_F(RtxFrameRingTest, theSlotHandedOutForRecordingIsNotOneAFrameInFlightHolds)
        {
            const bool countHits = false;
            FrameRing ring(getDevice(), countHits);

            // Filled to the brim: nothing collects, so every frame stays in flight, exactly as
            // `RtxTool::runWindow` leaves the ring.
            for (std::uint32_t frame = 0; frame < sFrameSlots; ++frame)
                submitEmpty(ring);

            EXPECT_EQ(ring.getRecording(), sFrameSlots) << "the ring did not take the frames";
            EXPECT_EQ(ring.slotOf(0).mState.get(), FrameState::Submitted)
                << "the first frame was finished by something";

            EXPECT_EQ(ring.recording().mState.get(), FrameState::Idle)
                << "the slot handed out is a frame still on the queue";

            // And the same answer to the same question, which is what a caller taking a graveyard
            // and then beginning the frame asks.
            EXPECT_EQ(&ring.recording(), &ring.begin()) << "beginning the frame moved to another slot";

            // Before the ring goes, because its command buffers go with it and the last frame is
            // still on the queue.
            ring.finishAll();
        }

        /// What is buried while a frame is in flight is held until a submit made after the burial
        /// has run, whoever buried it and whatever the ring was doing.
        ///
        /// **The property the two graveyards keyed by slot did not have.** A burial went into the
        /// recording frame's, and that frame's wait was what freed it — so a burial made into the
        /// wrong slot, or a table replaced by a caller that never saw a graveyard, was an object
        /// gone from under a trace. One graveyard stamps every burial with the next submit's value,
        /// so nothing is freed before every submit that could name it has finished.
        TEST_F(RtxFrameRingTest, aBurialOutlivesEverySubmitMadeBeforeIt)
        {
            // The device's graveyard is shared with every test before this one, so it is emptied
            // first: the counts below are of this test's burial alone.
            Graveyard& graveyard = getDevice().getGraveyard();
            getDevice().waitIdle();
            getDevice().collectIdle();

            FrameRing ring(getDevice(), false);

            // One frame on the queue, and a burial made while it is.
            submitEmpty(ring);
            graveyard.bury(Buffer::hostWritten(getDevice(), 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "test"));
            EXPECT_EQ(graveyard.getHeldCount(), 1u);

            // That frame done is not enough: the burial is stamped with the value of the submit
            // after it, which nothing has made.
            ring.finishAll();
            EXPECT_EQ(graveyard.getHeldCount(), 1u) << "freed before a submit made after the burial had run";

            // The next submit made and run is what frees it.
            submitEmpty(ring);
            ring.finishAll();
            EXPECT_EQ(graveyard.getHeldCount(), 0u) << "held past the submit that retired it";
        }

        /// A frame that left its picture in its slot comes back with it, and one that did not
        /// comes back with none. The picture is the slot's own memory: it stands until the ring
        /// comes round to the slot, which is what lets a caller read it off the report.
        TEST_F(RtxFrameRingTest, aFrameThatLeftItsPictureComesBackWithIt)
        {
            const Device& device = getDevice();
            FrameRing ring(device, false);

            // A frame that "copied" four bytes into its slot: the copy is the renderer's; what
            // the ring owes is the span over what the slot holds.
            constexpr std::array<std::uint8_t, 4> picture{ 1, 2, 3, 4 };
            FrameRecord& first = ring.begin();
            growTo(first.mReadBack, device, BufferKind::ReadBack, picture.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                "test picture");
            first.mReadBack.write(std::span<const std::uint8_t>(picture));
            first.mReadBackBytes = picture.size();
            getPool().begin(first.mWorld.mCommands);
            ring.submit(first);

            const std::optional<FrameResult> came = ring.collect();
            ASSERT_TRUE(came.has_value());
            EXPECT_EQ(came->mFrame, 0u);
            ASSERT_EQ(came->mPixels.size(), picture.size());
            EXPECT_TRUE(std::equal(came->mPixels.begin(), came->mPixels.end(), picture.begin()));

            // The next frame asked for nothing, and says so; the number counts on.
            submitEmpty(ring);
            const std::optional<FrameResult> next = ring.collect();
            ASSERT_TRUE(next.has_value());
            EXPECT_EQ(next->mFrame, 1u);
            EXPECT_TRUE(next->mPixels.empty()) << "a frame that asked for no picture came back with one";

            // And a slot begun again forgets what its last frame left, so a stale picture cannot
            // be read off a frame that did not ask.
            FrameRecord& third = ring.begin();
            EXPECT_EQ(third.mReadBackBytes, 0u);
            getPool().begin(third.mWorld.mCommands);
            ring.submit(third);
            ring.finishAll();
        }
    }
}
