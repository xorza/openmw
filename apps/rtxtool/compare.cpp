#include "compare.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <ostream>
#include <string>
#include <string_view>

#include <components/debug/debugging.hpp>
#include <components/files/conversion.hpp>

namespace RtxTool
{
    namespace
    {
        std::ostream& out()
        {
            return Debug::getRawStdout();
        }

        /// How a difference reads on one line.
        std::string describe(const FrameDifference& difference)
        {
            if (difference.mMismatched)
                return "no reference, or one of a different size";

            if (difference.same())
                return "same";

            return std::format(
                "differs: worst {} of 255 on {:.2f}% of the pixels", difference.mWorst, difference.getPercent());
        }
    }

    double FrameDifference::getPercent() const
    {
        return mTotal == 0 ? 0.0 : static_cast<double>(mDiffering) / static_cast<double>(mTotal) * 100.0;
    }

    FrameDifference compareFrames(const Rtx::PngImage& before, const Rtx::PngImage& after)
    {
        if (before.empty() || after.empty() || before.mWidth != after.mWidth || before.mHeight != after.mHeight)
            return FrameDifference{ .mMismatched = true };

        FrameDifference difference;
        difference.mTotal = std::uint64_t{ before.mWidth } * before.mHeight;

        for (std::size_t at = 0; at + 3 < before.mPixels.size(); at += 4)
        {
            std::uint32_t worst = 0;
            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                const auto one = static_cast<std::int32_t>(before.mPixels[at + channel]);
                const auto other = static_cast<std::int32_t>(after.mPixels[at + channel]);
                worst = std::max(worst, static_cast<std::uint32_t>(std::abs(one - other)));
            }

            if (worst > 0)
            {
                ++difference.mDiffering;
                difference.mWorst = std::max(difference.mWorst, worst);
            }
        }

        return difference;
    }

    int compareRuns(const std::filesystem::path& wrote, const std::filesystem::path& against,
        const std::span<const std::string> files, const std::span<const std::string> frames)
    {
        if (against.empty())
            return 0;

        out() << std::format("{} {} against {}\n", files.size(), files.size() == 1 ? "picture" : "pictures",
            Files::pathToUnicodeString(against));

        std::uint32_t differing = 0;
        std::uint32_t unmatched = 0;

        for (const std::string& file : files)
        {
            const Rtx::PngImage drawn = Rtx::readPng(wrote / file);
            const Rtx::PngImage reference = Rtx::readPng(against / file);
            const FrameDifference difference = compareFrames(reference, drawn);
            const bool frame = std::find(frames.begin(), frames.end(), file) != frames.end();

            out() << std::format("  {:<36} {}{}\n", file, describe(difference),
                frame && !difference.same() ? ", which the hashes judge" : "");

            if (frame)
                continue;

            if (difference.mMismatched)
                ++unmatched;
            else if (!difference.same())
                ++differing;
        }

        const std::size_t judged = files.size() - frames.size();
        const std::string_view frameNote = frames.empty() ? "" : "; the frames are judged by their hashes above";

        if (differing == 0 && unmatched == 0)
        {
            out() << std::format("  every picture judged here is the same{}\n", frameNote);
            return 0;
        }

        out() << std::format("  {} of {} pictures moved, {} had nothing to compare against{}\n", differing, judged,
            unmatched, frameNote);

        return 1;
    }
}
