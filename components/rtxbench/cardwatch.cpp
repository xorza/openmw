#include "cardwatch.hpp"

#include <algorithm>
#include <format>

#include <components/platform/process.hpp>

namespace Rtx
{
    void CardTally::take(const std::uint32_t pid, const std::string_view name)
    {
        ++mSamples;
        if (pid == mSelf)
            return;

        ++mOthers;

        const auto known = std::find_if(
            mHolders.begin(), mHolders.end(), [pid](const CardHolder& holder) { return holder.mPid == pid; });
        if (known != mHolders.end())
        {
            ++known->mSamples;
            return;
        }

        mHolders.push_back(CardHolder{ .mPid = pid, .mName = std::string(name), .mSamples = 1 });
    }

    void CardTally::clear()
    {
        mSamples = 0;
        mOthers = 0;
        mHolders.clear();
    }

    CardShare CardTally::summarise(const double seconds) const
    {
        CardShare share{
            .mSeconds = seconds,
            .mSamples = mSamples,
            .mOthers = mOthers,
            .mHolders = mHolders,
            .mViewed = true,
        };

        // Most samples first, and by name among equals, so two runs print the same line.
        std::sort(share.mHolders.begin(), share.mHolders.end(), [](const CardHolder& left, const CardHolder& right) {
            if (left.mSamples != right.mSamples)
                return left.mSamples > right.mSamples;
            return left.mName < right.mName;
        });

        return share;
    }

    CardWatch::CardWatch()
        : mTally(Platform::Process::currentId())
        , mBegan(std::chrono::steady_clock::now())
    {
    }

    CardWatch::~CardWatch() = default;

    void CardWatch::watch()
    {
        // **Ten a second.** The driver samples five times a second and hands back one sample a
        // process, the latest, so a poll slower than that loses the samples between; and a
        // reading is a few calls into the driver, so a faster poll costs nothing worth
        // measuring.
        constexpr std::chrono::milliseconds sPeriod{ 100 };

        if (mWorker.isRunning())
            return;

        mMonitor.under([&] { close(); });
        mWorker.repeat(sPeriod, [this] { mMonitor.under([this] { read(); }); });
    }

    CardShare CardWatch::start()
    {
        return mMonitor.under([&] { return close().mShare; });
    }

    CardReading CardWatch::stop()
    {
        return mMonitor.under([&] { return close(); });
    }

    std::uint32_t CardWatch::getReadings()
    {
        return mMonitor.under([this] { return mClock.mReadings; });
    }

    void CardWatch::read()
    {
        mClock.add(mNvml.readClock());

        mNvml.readSamples(mSamples);
        for (const CardSample& sample : mSamples)
        {
            if (!sample.mHeld)
                continue;

            mNvml.nameProcess(sample.mPid, mName);
            mTally.take(sample.mPid, mName);
        }
    }

    CardReading CardWatch::close()
    {
        read();

        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        CardReading reading{
            .mClock = mClock,
            .mShare = mTally.summarise(std::chrono::duration<double>(now - mBegan).count()),
        };
        if (!mNvml.hasSamples())
        {
            reading.mShare.mViewed = false;
            reading.mShare.mWhyNot = mNvml.describeUnsampled();
        }

        mClock = GpuClock{};
        mTally.clear();
        mBegan = now;

        return reading;
    }

    std::string describeCard(const CardShare& share)
    {
        if (!share.mViewed)
            return share.mWhyNot.empty() ? std::string("card not watched")
                                         : std::format("card not watched: {}", share.mWhyNot);

        // A window the driver took no sample in says so rather than claiming quiet: it samples
        // five times a second, and a stop of two frames is over before it does.
        if (share.mSamples == 0)
            return std::format("card not sampled over {:.1f} s", share.mSeconds);

        const char* const samples = share.mSamples == 1 ? "sample" : "samples";
        if (share.mOthers == 0)
            return std::format(
                "card held by no other process, {} {} over {:.1f} s", share.mSamples, samples, share.mSeconds);

        std::string named;
        for (const CardHolder& holder : share.mHolders)
            named += std::format("{}{} {}", named.empty() ? "" : ", ", holder.mName, holder.mSamples);

        return std::format("card held by another process in {} of {} {} over {:.1f} s: {}", share.mOthers,
            share.mSamples, samples, share.mSeconds, named);
    }
}
