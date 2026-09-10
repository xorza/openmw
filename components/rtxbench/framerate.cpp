#include "framerate.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <format>

namespace Rtx
{
    namespace
    {
        /// How much frame time closes a line. A second, so the figure moves as often as a clock's.
        constexpr double sLineMs = 1000.0;
    }

    bool FrameRate::add(const double frameMs)
    {
        mSummedMs += frameMs;
        mWorstMs = std::max(mWorstMs, frameMs);
        ++mFrames;

        if (mSummedMs < sLineMs)
            return false;

        const double meanMs = mSummedMs / mFrames;
        const auto [end, length] = std::format_to_n(
            mText.data(), mText.size(), "{:.0f} fps, {:.1f} ms, worst {:.1f} ms", sLineMs / meanMs, meanMs, mWorstMs);

        // A line is under forty characters at any rate a frame can have, so the buffer is not a
        // limit anything reaches — but a truncated line is still a line, and not an overrun.
        assert(static_cast<std::size_t>(length) <= mText.size() && "the frame rate line outgrew its buffer");
        mLength = std::min(static_cast<std::size_t>(length), mText.size());

        mSummedMs = 0.0;
        mWorstMs = 0.0;
        mFrames = 0;

        return true;
    }
}
