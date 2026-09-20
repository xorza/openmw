#include "codesettle.hpp"

#include <format>
#include <span>

namespace Rtx
{
    CodeSettle::CodeSettle(const double capSeconds)
        : mCapSeconds(capSeconds)
    {
    }

    void CodeSettle::judge(const ThreadWindows& windows)
    {
        if (mSettledAt.has_value())
            return;

        mSeconds = windows.getSeconds();
        mView = windows.hasView();
        mBusiestTotal = windows.getBusiestTotal();
        mShares.clear();
        for (const ThreadWindows::Window& window : windows.getWindows())
            mShares.push_back(window.mShare);

        // Over every window each time, which is a dozen numbers: the verdict is a function of the
        // whole sequence and never of how often it was asked.
        mSeenBusy = false;
        int quiet = 0;
        for (const ThreadWindows::Window& window : windows.getWindows())
        {
            if (window.mShare >= sBusyShare)
            {
                mSeenBusy = true;
                quiet = 0;
            }
            else if (mSeenBusy && ++quiet == sQuietWindows)
            {
                mSettledAt = window.mSeconds;
                return;
            }
        }
    }

    bool CodeSettle::isSettled() const
    {
        return mSettledAt.has_value() || mSeconds >= mCapSeconds;
    }

    std::string CodeSettle::describe() const
    {
        if (!mView)
            return std::format(
                "no view of the process's other threads on this platform, so the cap of {:.0f} s stood", mCapSeconds);

        if (mSettledAt.has_value())
            return std::format(
                "the busiest other thread went quiet at {:.1f} s after {:.1f} s of CPU", *mSettledAt, mBusiestTotal);

        // The windows themselves, because a cap that stood is a question: whether the thread
        // never finished or something else was busy after it did.
        std::string windows;
        for (const double share : mShares)
            windows += std::format("{}{:.2f}", windows.empty() ? "" : " ", share);

        if (!mSeenBusy)
            return std::format(
                "no other thread was busy in {:.1f} s, so the cap stood; the windows were {}", mSeconds, windows);

        return std::format(
            "the busiest other thread had no quiet window in {:.1f} s ({:.1f} s of CPU), so the cap stood; the windows "
            "were {}",
            mSeconds, mBusiestTotal, windows);
    }
}
