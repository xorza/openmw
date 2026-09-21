#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <vulkan/vulkan_core.h>

#include <components/rtx/latencyreport.hpp>
#include <components/rtx/pacing.hpp>
#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/latencypacer.hpp>

namespace Rtx
{
    namespace
    {
        /// What the stand-ins record: every call the pacer makes, in order, with what it passed.
        /// One of these, reached through the free functions the table names, because a function
        /// pointer captures nothing.
        struct Recorded
        {
            std::vector<std::pair<std::uint64_t, VkLatencyMarkerNV>> mMarkers;
            std::vector<VkLatencySleepModeInfoNV> mModes;
            std::vector<std::uint64_t> mSleepValues;
            std::uint32_t mWaits = 0;
            std::uint32_t mTimingsRead = 0;

            VkResult mModeAnswer = VK_SUCCESS;

            /// What `getTimings` hands back, as the driver would fill its ring.
            std::vector<VkLatencyTimingsFrameReportNV> mTimings;

            void clear() { *this = Recorded{}; }
        };

        Recorded& recorded()
        {
            static Recorded held;
            return held;
        }

        VKAPI_ATTR VkResult VKAPI_CALL setSleepMode(VkDevice, VkSwapchainKHR, const VkLatencySleepModeInfoNV* info)
        {
            recorded().mModes.push_back(*info);
            return recorded().mModeAnswer;
        }

        VKAPI_ATTR VkResult VKAPI_CALL sleep(VkDevice, VkSwapchainKHR, const VkLatencySleepInfoNV* info)
        {
            recorded().mSleepValues.push_back(info->value);
            return VK_SUCCESS;
        }

        VKAPI_ATTR void VKAPI_CALL setMarker(VkDevice, VkSwapchainKHR, const VkSetLatencyMarkerInfoNV* info)
        {
            recorded().mMarkers.emplace_back(info->presentID, info->marker);
        }

        VKAPI_ATTR void VKAPI_CALL getTimings(VkDevice, VkSwapchainKHR, VkGetLatencyMarkerInfoNV* info)
        {
            ++recorded().mTimingsRead;
            const std::vector<VkLatencyTimingsFrameReportNV>& held = recorded().mTimings;
            const std::size_t count = std::min<std::size_t>(held.size(), info->timingCount);
            for (std::size_t at = 0; at < count; ++at)
                info->pTimings[at] = held[at];
            info->timingCount = static_cast<std::uint32_t>(count);
        }

        VKAPI_ATTR VkResult VKAPI_CALL waitSemaphores(VkDevice, const VkSemaphoreWaitInfo*, std::uint64_t)
        {
            ++recorded().mWaits;
            return VK_SUCCESS;
        }

        const LatencyFunctions sStandIns{
            .mSetSleepMode = setSleepMode,
            .mSleep = sleep,
            .mSetMarker = setMarker,
            .mGetTimings = getTimings,
            .mWaitSemaphores = waitSemaphores,
        };

        /// Handles the stand-ins never dereference, distinct so a swapchain replaced is a
        /// different value.
        const VkDevice sDevice = reinterpret_cast<VkDevice>(0x10);
        const VkSemaphore sSemaphore = reinterpret_cast<VkSemaphore>(0x20);
        const VkSwapchainKHR sSwapchain = reinterpret_cast<VkSwapchainKHR>(0x30);
        const VkSwapchainKHR sRemade = reinterpret_cast<VkSwapchainKHR>(0x40);

        class RtxLatencyPacerTest : public ::testing::Test
        {
        protected:
            void SetUp() override { recorded().clear(); }

            /// The markers set since the last take, as (id, marker) pairs.
            std::vector<std::pair<std::uint64_t, VkLatencyMarkerNV>> takeMarkers()
            {
                return std::exchange(recorded().mMarkers, {});
            }

            LatencyPacer mPacer{ sStandIns, sDevice, sSemaphore };
        };

        using Markers = std::vector<std::pair<std::uint64_t, VkLatencyMarkerNV>>;

        /// **A frame is one sleep to one present, and every present carries its markers.** The
        /// order the driver is told, from the first sleep on: nothing before it, the two start
        /// markers with it, the simulation's end and the submission's start where the game hands
        /// over, the four around the present, and the id every marker of a frame shares. A second
        /// sleep between two presents is refused, because the driver counts one.
        TEST_F(RtxLatencyPacerTest, aFrameRunsFromItsSleepToItsPresentUnderOneId)
        {
            mPacer.follow(sSwapchain, true);
            ASSERT_TRUE(mPacer.isLive());
            EXPECT_EQ(recorded().mModes.size(), 1u) << "the mode is the swapchain's, applied as it is followed";
            EXPECT_TRUE(takeMarkers().empty()) << "a marker before the first sleep";
            EXPECT_EQ(mPacer.getPresentId(), 1u) << "the id the first frame will open with";

            mPacer.awaitFrame();
            EXPECT_EQ(recorded().mSleepValues, (std::vector<std::uint64_t>{ 1 }));
            EXPECT_EQ(recorded().mWaits, 1u);
            EXPECT_EQ(takeMarkers(),
                (Markers{ { 1, VK_LATENCY_MARKER_SIMULATION_START_NV }, { 1, VK_LATENCY_MARKER_INPUT_SAMPLE_NV } }));
            EXPECT_EQ(mPacer.getPresentId(), 1u);

            // The window went hidden, or the present failed: the frame is still open.
            mPacer.awaitFrame();
            EXPECT_EQ(recorded().mSleepValues.size(), 1u) << "a second sleep between two presents";
            EXPECT_TRUE(takeMarkers().empty());

            mPacer.endSimulation(false);
            EXPECT_EQ(takeMarkers(),
                (Markers{
                    { 1, VK_LATENCY_MARKER_SIMULATION_END_NV }, { 1, VK_LATENCY_MARKER_RENDERSUBMIT_START_NV } }));
            mPacer.endSimulation(false);
            EXPECT_TRUE(takeMarkers().empty()) << "a simulation ended twice";

            mPacer.beforePresent();
            EXPECT_EQ(takeMarkers(),
                (Markers{ { 1, VK_LATENCY_MARKER_RENDERSUBMIT_END_NV }, { 1, VK_LATENCY_MARKER_PRESENT_START_NV } }));
            EXPECT_EQ(mPacer.getPresentId(), 1u) << "the present carries the frame's id";
            mPacer.afterPresent();
            EXPECT_EQ(takeMarkers(), (Markers{ { 1, VK_LATENCY_MARKER_PRESENT_END_NV } }));
            EXPECT_EQ(recorded().mTimingsRead, 1u);
            EXPECT_EQ(mPacer.getPresentId(), 2u) << "what the next submit is stamped with";

            // The next frame: the sleep owed again, the id up by one.
            mPacer.awaitFrame();
            EXPECT_EQ(recorded().mSleepValues, (std::vector<std::uint64_t>{ 1, 2 }));
            EXPECT_EQ(takeMarkers(),
                (Markers{ { 2, VK_LATENCY_MARKER_SIMULATION_START_NV }, { 2, VK_LATENCY_MARKER_INPUT_SAMPLE_NV } }));

            // The flash rides the frame a click landed in, ahead of the simulation's end.
            mPacer.endSimulation(true);
            EXPECT_EQ(takeMarkers(),
                (Markers{ { 2, VK_LATENCY_MARKER_TRIGGER_FLASH_NV }, { 2, VK_LATENCY_MARKER_SIMULATION_END_NV },
                    { 2, VK_LATENCY_MARKER_RENDERSUBMIT_START_NV } }));
        }

        /// **A present nothing slept for pays its sleep first**, at the spot it is, with the whole
        /// marker set: a loading screen's present is a frame the driver counts. And a swapchain
        /// remade inside a frame keeps the frame open and the ids going up, never back.
        TEST_F(RtxLatencyPacerTest, aPresentWithNoSleepPaysItAndARemadeSwapchainKeepsTheCount)
        {
            mPacer.follow(sSwapchain, true);

            mPacer.beforePresent();
            EXPECT_EQ(recorded().mSleepValues.size(), 1u) << "the owed sleep, paid before the acquire";
            EXPECT_EQ(takeMarkers(),
                (Markers{ { 1, VK_LATENCY_MARKER_SIMULATION_START_NV }, { 1, VK_LATENCY_MARKER_INPUT_SAMPLE_NV },
                    { 1, VK_LATENCY_MARKER_SIMULATION_END_NV }, { 1, VK_LATENCY_MARKER_RENDERSUBMIT_START_NV },
                    { 1, VK_LATENCY_MARKER_RENDERSUBMIT_END_NV }, { 1, VK_LATENCY_MARKER_PRESENT_START_NV } }));
            mPacer.afterPresent();
            EXPECT_EQ(takeMarkers(), (Markers{ { 1, VK_LATENCY_MARKER_PRESENT_END_NV } }));

            // Frame two opens, and a resize remakes the swapchain inside it.
            mPacer.awaitFrame();
            EXPECT_EQ(mPacer.getPresentId(), 2u);
            mPacer.follow(sRemade, true);
            EXPECT_EQ(recorded().mModes.size(), 2u) << "the mode does not survive a rebuild, so it is applied again";
            EXPECT_TRUE(mPacer.isLive());
            mPacer.awaitFrame();
            EXPECT_EQ(recorded().mSleepValues.size(), 2u) << "the frame the resize happened in is still open";
            takeMarkers();

            mPacer.beforePresent();
            mPacer.afterPresent();
            EXPECT_EQ(takeMarkers().back(), (std::pair{ std::uint64_t{ 2 }, VK_LATENCY_MARKER_PRESENT_END_NV }));
            EXPECT_EQ(mPacer.getPresentId(), 3u);
        }

        /// **Dormant is every call nothing and an id of nought**, which chains nothing: a surface
        /// that paces no mode, and a driver that refuses the mode. A pacer that comes back holds no
        /// frame from before.
        TEST_F(RtxLatencyPacerTest, aPacerThatPacesNothingSaysNothingAndComesBackFresh)
        {
            mPacer.follow(sSwapchain, false);
            EXPECT_FALSE(mPacer.isLive());
            EXPECT_EQ(mPacer.getPresentId(), 0u);
            mPacer.awaitFrame();
            mPacer.endSimulation(true);
            mPacer.beforePresent();
            mPacer.afterPresent();
            EXPECT_TRUE(recorded().mModes.empty());
            EXPECT_TRUE(recorded().mSleepValues.empty());
            EXPECT_TRUE(takeMarkers().empty());
            EXPECT_FALSE(mPacer.describeLatency().has_value());

            // Live, a frame opened, then dormant across a mode change and live again: the frame
            // that was open closed nowhere the driver could see, so the next sleep is owed.
            mPacer.follow(sSwapchain, true);
            mPacer.awaitFrame();
            EXPECT_EQ(recorded().mSleepValues.size(), 1u);
            mPacer.follow(sRemade, false);
            EXPECT_EQ(mPacer.getPresentId(), 0u);
            mPacer.follow(sRemade, true);
            mPacer.awaitFrame();
            EXPECT_EQ(recorded().mSleepValues.size(), 2u) << "a frame held open across a dormant spell";

            // The driver refuses the mode: dormant, once said, and the frame still presents.
            recorded().mModeAnswer = VK_ERROR_INITIALIZATION_FAILED;
            mPacer.follow(sSwapchain, true);
            EXPECT_FALSE(mPacer.isLive());
            EXPECT_EQ(mPacer.getPresentId(), 0u);
        }

        /// **The mode is the three flags the setting spells, applied where live and once per
        /// change.** Off is the sleep as a limiter and nothing else; on is the low-latency flag;
        /// boost is that and the clock; the interval rides every one of them.
        TEST_F(RtxLatencyPacerTest, theModeIsAppliedAsTheSettingSpellsItAndOnlyOnAChange)
        {
            struct Case
            {
                LatencyMode mMode;
                VkBool32 mLowLatency;
                VkBool32 mBoost;
            };
            constexpr std::array<Case, 3> sCases{
                Case{ LatencyMode::Off, VK_FALSE, VK_FALSE },
                Case{ LatencyMode::On, VK_TRUE, VK_FALSE },
                Case{ LatencyMode::Boost, VK_TRUE, VK_TRUE },
            };

            // Set before the swapchain is followed: kept, and applied with the follow.
            mPacer.setPacing(Pacing{ .mMode = LatencyMode::Boost, .mMinimumIntervalUs = 8333 });
            EXPECT_TRUE(recorded().mModes.empty()) << "nothing to apply to yet";
            mPacer.follow(sSwapchain, true);
            ASSERT_EQ(recorded().mModes.size(), 1u);
            EXPECT_EQ(recorded().mModes[0].lowLatencyBoost, VK_TRUE);
            EXPECT_EQ(recorded().mModes[0].minimumIntervalUs, 8333u);

            for (const Case& expected : sCases)
            {
                recorded().mModes.clear();
                mPacer.setPacing(Pacing{ .mMode = expected.mMode, .mMinimumIntervalUs = 4000 });
                ASSERT_EQ(recorded().mModes.size(), 1u) << sLatencyModeNames.name(expected.mMode);
                EXPECT_EQ(recorded().mModes[0].lowLatencyMode, expected.mLowLatency);
                EXPECT_EQ(recorded().mModes[0].lowLatencyBoost, expected.mBoost);
                EXPECT_EQ(recorded().mModes[0].minimumIntervalUs, 4000u);

                mPacer.setPacing(Pacing{ .mMode = expected.mMode, .mMinimumIntervalUs = 4000 });
                EXPECT_EQ(recorded().mModes.size(), 1u) << "the same pacing applied again";
            }
        }

        /// **The report is the newest frame in the ring, wherever the driver put it**, as the
        /// stretch from its input sample to the end of its present; a stretch the driver never saw
        /// the start of reads as nought.
        TEST_F(RtxLatencyPacerTest, theReportIsTheNewestFrameInTheDriversRing)
        {
            const auto frame = [](const std::uint64_t id, const std::uint64_t base) {
                VkLatencyTimingsFrameReportNV timing{ .sType = VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV };
                timing.presentID = id;
                timing.inputSampleTimeUs = base + 100;
                timing.presentEndTimeUs = base + 5300;
                return timing;
            };

            // The newest second in the ring, behind an older one and ahead of an empty slot.
            recorded().mTimings = { frame(3, 30000), frame(7, 70000), VkLatencyTimingsFrameReportNV{} };

            mPacer.follow(sSwapchain, true);
            mPacer.awaitFrame();
            mPacer.beforePresent();
            mPacer.afterPresent();

            const std::optional<LatencyReport> report = mPacer.describeLatency();
            ASSERT_TRUE(report.has_value());
            EXPECT_EQ(report->mPresentId, 7u);
            EXPECT_EQ(report->mInputToPresentUs, 5200u) << "5300 - 100";

            // A frame whose input the driver never stamped: no input-to-present figure.
            recorded().mTimings = { frame(8, 80000) };
            recorded().mTimings[0].inputSampleTimeUs = 0;
            mPacer.awaitFrame();
            mPacer.beforePresent();
            mPacer.afterPresent();
            ASSERT_TRUE(mPacer.describeLatency().has_value());
            EXPECT_EQ(mPacer.describeLatency()->mPresentId, 8u);
            EXPECT_EQ(mPacer.describeLatency()->mInputToPresentUs, 0u);
        }
    }
}
