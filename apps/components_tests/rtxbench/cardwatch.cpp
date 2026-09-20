#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <components/platform/process.hpp>
#include <components/rtxbench/cardwatch.hpp>
#include <components/rtxbench/nvml.hpp>

namespace Rtx
{
    namespace
    {
        /// A tally counts every sample, tells its own process's from the rest, keeps a process's
        /// first name, and orders the others so two runs print the same line.
        TEST(RtxCardTallyTest, theOthersAreCountedByProcessAndOrderedMostFirst)
        {
            CardTally tally(100);

            // Seven samples: three of this process, two of 7, one each of 12 and 30, with 30 seen
            // once under one name and 12 renamed on its second sample.
            tally.take(100, "openmw-rtxtool");
            tally.take(7, "kwin_wayland");
            tally.take(100, "openmw-rtxtool");
            tally.take(30, "brave");
            tally.take(12, "zed-editor");
            tally.take(7, "kwin_wayland");
            tally.take(100, "openmw-rtxtool");

            const CardShare share = tally.summarise(2.5);
            EXPECT_TRUE(share.mViewed);
            EXPECT_DOUBLE_EQ(share.mSeconds, 2.5);
            EXPECT_EQ(share.mSamples, 7u);
            EXPECT_EQ(share.mOthers, 4u) << "7 twice, 12 once, 30 once";

            // Most samples first, and among equals by name: brave before zed-editor.
            ASSERT_EQ(share.mHolders.size(), 3u);
            EXPECT_EQ(share.mHolders[0].mName, "kwin_wayland");
            EXPECT_EQ(share.mHolders[0].mSamples, 2u);
            EXPECT_EQ(share.mHolders[1].mName, "brave");
            EXPECT_EQ(share.mHolders[1].mSamples, 1u);
            EXPECT_EQ(share.mHolders[2].mName, "zed-editor");
            EXPECT_EQ(share.mHolders[2].mSamples, 1u);

            EXPECT_EQ(describeCard(share),
                "card held by another process in 4 of 7 samples over 2.5 s: kwin_wayland 2, brave 1, zed-editor 1");

            // A process is named by its first sample, so one that is gone by the report — which
            // the driver then calls by number — is still the name it had.
            tally.take(12, "pid 12");
            EXPECT_EQ(tally.summarise(3.0).mHolders[0].mName, "kwin_wayland");
            EXPECT_EQ(tally.summarise(3.0).mHolders[1].mName, "zed-editor") << "two samples now, and its first name";

            // Cleared, the tally forgets the samples and keeps its process.
            tally.clear();
            tally.take(100, "openmw-rtxtool");
            const CardShare alone = tally.summarise(1.0);
            EXPECT_EQ(alone.mSamples, 1u);
            EXPECT_EQ(alone.mOthers, 0u);
            EXPECT_TRUE(alone.mHolders.empty());
            EXPECT_EQ(describeCard(alone), "card held by no other process, 1 sample over 1.0 s");

            // A window the driver took no sample in claims nothing about who held the card.
            tally.clear();
            EXPECT_EQ(describeCard(tally.summarise(0.03)), "card not sampled over 0.0 s");

            // A share nothing looked at says why where it can.
            CardShare blind;
            EXPECT_EQ(describeCard(blind), "card not watched");
            blind.mWhyNot = "the driver keeps no process samples for this device";
            EXPECT_EQ(describeCard(blind), "card not watched: the driver keeps no process samples for this device");
        }

        /// A process is named by its executable alone, whatever else the driver hands back.
        TEST(RtxNvmlTest, aProcessIsNamedByItsExecutable)
        {
            EXPECT_EQ(Nvml::executableOf("/usr/bin/kwin_wayland"), "kwin_wayland");
            EXPECT_EQ(Nvml::executableOf("./components-tests"), "components-tests");
            EXPECT_EQ(Nvml::executableOf("zed-editor"), "zed-editor");
            EXPECT_EQ(Nvml::executableOf(R"(C:\Program Files\Zed\zed.exe)"), "zed.exe") << "a space in a directory";

            // A browser's GPU process titles itself with its whole command line, whose last
            // slash is inside an argument and whose first option is where the path ends.
            EXPECT_EQ(Nvml::executableOf("/opt/brave-bin/brave --type=gpu-process "
                                         "--render-node-override=/dev/dri/renderD128 --crashpad-handler-pid=1"),
                "brave");
            EXPECT_EQ(Nvml::executableOf(""), "");
        }

        /// What the driver's library answers on this machine, where it is there at all.
        ///
        /// **A skip and not a failure where nothing answers**: the tests run on machines without
        /// an NVIDIA driver, and the card's clock is instrumentation rather than a renderer.
        TEST(RtxNvmlTest, theLibraryAnswersAClockAndNamesThisProcess)
        {
            Nvml nvml;
            if (!nvml.isOpen())
                GTEST_SKIP() << "no driver library on this machine: " << nvml.describeAbsence();

            EXPECT_TRUE(nvml.describeAbsence().empty());

            // A graphics clock and a memory clock a card of the last decade could hold, which is
            // what says the fields were read in the order they were asked for rather than shuffled.
            const GpuClock clock = nvml.readClock();
            ASSERT_TRUE(clock.mRead);
            EXPECT_GT(clock.mLowestMhz, 100u);
            EXPECT_LT(clock.mLowestMhz, 10000u);
            EXPECT_EQ(clock.mLowestMhz, clock.mHighestMhz) << "one reading is not a range";
            EXPECT_GT(clock.mMemoryMhz, 100u);
            EXPECT_GT(clock.mTemperatureC, 0u);
            EXPECT_LT(clock.mTemperatureC, 120u);

            // This process, by the name the driver keeps for it: the executable's own, without
            // its directory, which is what a report prints beside a count.
            std::string name;
            nvml.nameProcess(Platform::Process::currentId(), name);
            EXPECT_EQ(name, "components-tests") << name;

            // One nobody runs is named by its number.
            nvml.nameProcess(4'000'000'000u, name);
            EXPECT_EQ(name, "pid 4000000000");

            // Whatever the driver has sampled since the library opened is newer than what it held
            // then, so nothing that came back predates the opening.
            if (nvml.hasSamples())
            {
                std::vector<CardSample> samples;
                nvml.readSamples(samples);
                for (const CardSample& sample : samples)
                    EXPECT_GT(sample.mStamp, 0u);
            }
        }

        /// A watch samples across the window it is open for, and every window starts from
        /// nothing.
        ///
        /// **The reading count is the whole claim for the clock.** What the row said before this
        /// existed was two samples printed as a range, and a reader could not tell that from a
        /// card watched throughout — so what a watch has to prove is that it took more than two.
        TEST(RtxCardWatchTest, aWatchSamplesAcrossTheWindowAndEveryWindowStartsFromNothing)
        {
            const bool sampled = Nvml().hasSamples();
            if (!Nvml().isOpen())
                GTEST_SKIP() << "no driver library on this machine";

            CardWatch watch;
            watch.watch();

            // **Waited for and not slept out.** What the claim needs is a few turns of the watch's
            // own loop, and how long those take is the machine's to say.
            const auto waitFor = [&](std::uint32_t readings) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (watch.getReadings() < readings && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
            };

            waitFor(2);
            ASSERT_GE(watch.getReadings(), 2u) << "the watch's loop never came round";

            // A second `watch` is nothing at all: the window goes on counting.
            watch.watch();
            EXPECT_GE(watch.getReadings(), 2u);

            // Starting a place's window answers the window before it and begins from nothing.
            const CardShare before = watch.start();
            EXPECT_EQ(before.mViewed, sampled);
            EXPECT_GT(before.mSeconds, 0.0);
            EXPECT_LT(watch.getReadings(), 2u) << "a place's window carried the window before it";

            waitFor(2);
            const CardReading place = watch.stop();
            EXPECT_TRUE(place.mClock.mRead);
            EXPECT_GT(place.mClock.mReadings, 2u) << "a watch that answered with no more than its two ends";
            EXPECT_GE(place.mClock.getMeanMhz(), place.mClock.mLowestMhz);
            EXPECT_LE(place.mClock.getMeanMhz(), place.mClock.mHighestMhz);
            EXPECT_EQ(place.mShare.mViewed, sampled);
            EXPECT_GT(place.mShare.mSeconds, 0.0);

            // And the window after a stop is a window of its own, shorter than the place's.
            const CardShare after = watch.start();
            EXPECT_LT(after.mSeconds, place.mShare.mSeconds);
        }
    }
}
