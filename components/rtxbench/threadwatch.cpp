#include "threadwatch.hpp"

#include <algorithm>
#include <cassert>
#include <format>

#ifdef __linux__
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <string_view>
#include <system_error>

#include <dirent.h>
#include <unistd.h>
#endif

namespace Rtx
{
    void ThreadWindows::take(const double seconds, const std::span<const ThreadCpu> threads)
    {
        assert(std::is_sorted(threads.begin(), threads.end(), [](const ThreadCpu& left, const ThreadCpu& right) {
            return left.mThread < right.mThread;
        }) && "threads out of order");

        mSeconds = seconds;

        if (threads.empty())
            return;

        if (!mWindow.has_value())
        {
            mWindow = Anchor{ .mSeconds = seconds, .mThreads = { threads.begin(), threads.end() } };
            return;
        }

        if (seconds - mWindow->mSeconds >= sWindowSeconds)
            close(seconds, threads);
    }

    void ThreadWindows::finish(const double seconds, const std::span<const ThreadCpu> threads)
    {
        take(seconds, threads);

        if (mWindow.has_value() && !threads.empty() && seconds - mWindow->mSeconds >= sShortestWindowSeconds)
            close(seconds, threads);
    }

    void ThreadWindows::close(const double seconds, const std::span<const ThreadCpu> threads)
    {
        const double length = seconds - mWindow->mSeconds;
        const double busiest = busiestSeconds(threads);

        mClosed.push_back(Window{ .mShare = busiest / length, .mSeconds = seconds });
        mBusiestTotal += busiest;

        mWindow->mSeconds = seconds;
        mWindow->mThreads.assign(threads.begin(), threads.end());
    }

    double ThreadWindows::busiestSeconds(const std::span<const ThreadCpu> threads) const
    {
        // Threads that live across the window, matched by id; one that began or ended inside it
        // is at most a window's worth, and the thread waited for lives for many.
        double busiest = 0.0;
        auto was = mWindow->mThreads.begin();
        for (const ThreadCpu& now : threads)
        {
            while (was != mWindow->mThreads.end() && was->mThread < now.mThread)
                ++was;
            if (was == mWindow->mThreads.end())
                break;
            if (was->mThread == now.mThread)
                busiest = std::max(busiest, now.mSeconds - was->mSeconds);
        }

        return busiest;
    }

    ThreadShare ThreadWindows::summarise() const
    {
        ThreadShare share{ .mWindows = static_cast<std::uint32_t>(mClosed.size()), .mViewed = hasView() };

        std::uint32_t run = 0;
        for (const Window& window : mClosed)
        {
            if (window.mShare > share.mLargestShare)
            {
                share.mLargestShare = window.mShare;
                share.mLargestAt = window.mSeconds;
            }

            run = window.mShare >= ThreadShare::sFlatOutShare ? run + 1 : 0;
            share.mLongestFlatOut = std::max(share.mLongestFlatOut, run);
        }

        return share;
    }

    std::string describeThreads(const ThreadShare& share)
    {
        if (!share.mViewed)
            return "no view of the process's other threads on this platform";

        if (share.mWindows == 0)
            return "no window of the process's other threads closed";

        std::string line
            = std::format("the busiest other thread took {:.2f} of a core at most, at {:.1f} s, over {} windows",
                share.mLargestShare, share.mLargestAt, share.mWindows);
        if (share.mLongestFlatOut > 0)
            line += std::format(", and held one flat out through {} in a row", share.mLongestFlatOut);

        return line;
    }

    ThreadWatch::~ThreadWatch() = default;

    void ThreadWatch::start(const std::uint64_t except)
    {
        // Four a second: a window is two seconds, and a reading is thirty small files.
        constexpr std::chrono::milliseconds sPeriod{ 250 };

        // Set before the thread that reads them exists, and only where no thread does: a watch
        // held across the places of a suite must not hand the second place the first place's
        // windows, nor clear a run in progress.
        if (mWorker.isRunning())
            return;

        mExcept = except;
        mBegan = std::chrono::steady_clock::now();
        mMonitor.under([&] { mWindows = ThreadWindows{}; });

        mWorker.repeat(sPeriod, [this] { read(); });
    }

    void ThreadWatch::read()
    {
        readOtherThreads(mThreads, mExcept);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - mBegan).count();
        mMonitor.under([&] { mWindows.take(seconds, mThreads); });
    }

    ThreadWindows ThreadWatch::stop()
    {
        mWorker.stop();

        // One more after the join, which is the sampler's own thread gone, so the last frames are
        // covered by a reading taken after them; and the window still open is closed by it, so a
        // place shorter than a window still answers.
        readOtherThreads(mThreads, mExcept);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - mBegan).count();
        return mMonitor.under([&] {
            mWindows.finish(seconds, mThreads);
            return mWindows;
        });
    }

    std::uint64_t ThreadWatch::currentThread()
    {
#ifdef __linux__
        return static_cast<std::uint64_t>(gettid());
#else
        return 0;
#endif
    }

    void ThreadWatch::readOtherThreads(std::vector<ThreadCpu>& into, const std::uint64_t except)
    {
        into.clear();

#ifdef __linux__
        // `/proc/self/task/<tid>/stat`: after the command's closing parenthesis, the fourteenth
        // and fifteenth fields are the thread's user and system time in clock ticks. Read with the
        // C library and a buffer on the stack, so a reading allocates nothing per thread.
        static const double sSecondsPerTick = 1.0 / static_cast<double>(sysconf(_SC_CLK_TCK));
        const auto self = static_cast<std::uint64_t>(gettid());

        DIR* const tasks = opendir("/proc/self/task");
        if (tasks == nullptr)
            return;

        std::array<char, 64> path{};
        std::array<char, 1024> line{};
        while (const dirent* const task = readdir(tasks))
        {
            const std::string_view name(task->d_name);
            std::uint64_t thread = 0;
            if (std::from_chars(name.data(), name.data() + name.size(), thread).ec != std::errc{} || thread == self
                || thread == except)
                continue;

            std::snprintf(
                path.data(), path.size(), "/proc/self/task/%llu/stat", static_cast<unsigned long long>(thread));
            std::FILE* const stat = std::fopen(path.data(), "r");
            if (stat == nullptr)
                continue;

            const std::size_t read = std::fread(line.data(), 1, line.size() - 1, stat);
            std::fclose(stat);

            const std::string_view text(line.data(), read);
            const std::size_t closed = text.rfind(')');
            if (closed == std::string_view::npos)
                continue;

            // Field 3 is the state, right after the parenthesis; fields 14 and 15 are the times.
            std::size_t at = closed + 1;
            std::uint64_t ticks = 0;
            bool whole = true;
            for (int field = 3; field <= 15 && whole; ++field)
            {
                while (at < text.size() && text[at] == ' ')
                    ++at;
                const std::size_t end = std::min(text.find(' ', at), text.size());
                if (field >= 14)
                {
                    std::uint64_t value = 0;
                    whole = std::from_chars(text.data() + at, text.data() + end, value).ec == std::errc{};
                    ticks += value;
                }
                at = end;
            }
            if (!whole)
                continue;

            into.push_back(ThreadCpu{ .mThread = thread, .mSeconds = static_cast<double>(ticks) * sSecondsPerTick });
        }
        closedir(tasks);

        std::sort(into.begin(), into.end(),
            [](const ThreadCpu& left, const ThreadCpu& right) { return left.mThread < right.mThread; });
#endif
    }
}
