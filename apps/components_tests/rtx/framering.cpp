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
            // a watched window (`view`) leaves the ring.
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

        /// A frame that left its picture comes back with it, and one that did not comes back with
        /// none. The picture stands past the frame that takes the slot: a report is collected
        /// before that frame is drawn and read after it, and the memory it reads is the frame
        /// after's to write over.
        TEST_F(RtxFrameRingTest, aPictureStandsPastTheFrameThatTakesItsSlot)
        {
            const Device& device = getDevice();
            FrameRing ring(device, false);

            constexpr std::array<std::uint8_t, 4> picture{ 1, 2, 3, 4 };
            constexpr std::array<std::uint8_t, 4> other{ 5, 6, 7, 8 };

            // A frame that "copied" four bytes: the copy is the renderer's; what the ring owes is
            // the memory and the span over it.
            const auto leave = [&](FrameRecord& frame, const std::span<const std::uint8_t> bytes) {
                Buffer& into = ring.pictureOf(ring.getRecording());
                growTo(
                    into, device, BufferKind::ReadBack, bytes.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, "test picture");
                into.write(bytes);
                frame.mReadBackBytes = bytes.size();
                getPool().begin(frame.mWorld.mCommands);
                ring.submit(frame);
            };

            leave(ring.begin(), picture);
            submitEmpty(ring);

            // Collected before the frame that takes its slot, as the renderer does.
            const std::optional<FrameResult> came = ring.collectFinished();
            ASSERT_TRUE(came.has_value());
            EXPECT_EQ(came->mFrame, 0u);
            ASSERT_EQ(came->mPixels.size(), picture.size());
            EXPECT_TRUE(std::equal(came->mPixels.begin(), came->mPixels.end(), picture.begin()));

            // The slot begun again forgets what its last frame left, so a stale picture cannot be
            // read off a frame that did not ask — and the picture it leaves lands elsewhere.
            FrameRecord& third = ring.begin();
            EXPECT_EQ(third.mReadBackBytes, 0u);
            EXPECT_NE(&ring.pictureOf(2), &ring.pictureOf(0)) << "the frame that took the slot took the picture";
            EXPECT_EQ(&ring.pictureOf(3), &ring.pictureOf(0)) << "the frame after is the one that takes it";
            leave(third, other);
            EXPECT_TRUE(std::equal(came->mPixels.begin(), came->mPixels.end(), picture.begin()))
                << "the frame that took the slot wrote over the picture";

            const std::optional<FrameResult> next = ring.collect();
            ASSERT_TRUE(next.has_value());
            EXPECT_EQ(next->mFrame, 1u);
            EXPECT_TRUE(next->mPixels.empty()) << "a frame that asked for no picture came back with one";

            const std::optional<FrameResult> after = ring.collect();
            ASSERT_TRUE(after.has_value());
            EXPECT_EQ(after->mFrame, 2u);
            ASSERT_EQ(after->mPixels.size(), other.size());
            EXPECT_TRUE(std::equal(after->mPixels.begin(), after->mPixels.end(), other.begin()));
        }

        /// **A frame the host refused to trace closes, and the next placement takes the next
        /// slot.** Placed and skipped frame after frame, each slot holds the one placement buffer
        /// its own frame took, where a frame left open took one more at every placement. A skipped
        /// frame is numbered and waited for and comes back with no report, so the traced frame
        /// after three of them is the one report, under the number it was submitted with.
        TEST_F(RtxFrameRingTest, aSkippedFrameClosesAndComesBackWithNoReport)
        {
            FrameRing ring(getDevice(), false);

            constexpr std::uint64_t skipped = 3;
            for (std::uint64_t at = 0; at < skipped; ++at)
            {
                FrameRecord& frame = ring.begin();
                const VkCommandBuffer placement = ring.takePlaceCommands(frame);
                getPool().begin(placement);
                getPool().submit(placement);

                EXPECT_TRUE(ring.isOpen());
                ring.skip();
                EXPECT_FALSE(ring.isOpen());
            }

            EXPECT_EQ(ring.getRecording(), skipped) << "a skipped frame went unnumbered";
            for (std::uint64_t frame = 0; frame < sFrameSlots; ++frame)
                EXPECT_EQ(ring.slotOf(frame).mPlaceCommands.size(), 1u) << "slot of frame " << frame;

            submitEmpty(ring);
            const std::optional<FrameResult> traced = ring.collect();
            ASSERT_TRUE(traced.has_value()) << "the skipped frames stood in front of the traced one";
            EXPECT_EQ(traced->mFrame, skipped);
            EXPECT_FALSE(ring.collect().has_value()) << "a skipped frame came back with a report";
        }
    }
}
