#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Rtx
{
    /// What the last second of frames came to, as one short line for a glance.
    ///
    /// **A second and not a frame**, because a figure that changes sixty times a second cannot be
    /// read. **The mean beside the worst**, because the mean alone is the figure that hides a
    /// stutter. A median would need the second's frames kept and sorted, and a sort landing on one
    /// frame in sixty is the spike a frame path is not allowed to carry.
    ///
    /// **Nothing here reaches the heap.** The line is formatted into a buffer this owns, so the
    /// frame that closes a second costs one format and every other costs three adds.
    class FrameRate
    {
    public:
        /// Takes one frame's wall time. True on the frame that closes a second's worth, at which
        /// point `getText` describes that second and the next one starts counting from nothing.
        bool add(double frameMs);

        /// The last closed second: `118 fps, 8.5 ms, worst 12.3 ms`. Empty until one has closed.
        std::string_view getText() const { return { mText.data(), mLength }; }

    private:
        double mSummedMs = 0.0;
        double mWorstMs = 0.0;
        std::uint32_t mFrames = 0;

        std::array<char, 64> mText{};
        std::size_t mLength = 0;
    };
}
