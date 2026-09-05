#include "verify.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <ostream>
#include <string>

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

        /// Where a view's frame is written, under whichever run directory.
        std::filesystem::path frameFile(const std::filesystem::path& directory, const std::string& view)
        {
            return directory / (view + ".png");
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

    int compareRuns(
        const std::filesystem::path& wrote, const std::filesystem::path& against, std::span<const View> views)
    {
        if (against.empty())
        {
            out() << "verify: no --against, so this run is only a reference for the next one\n";
            return 0;
        }

        out() << std::format("verify: {} {} against {}\n", views.size(), views.size() == 1 ? "view" : "views",
            Files::pathToUnicodeString(against));

        std::uint32_t differing = 0;
        std::uint32_t unmatched = 0;

        for (const View& view : views)
        {
            const Rtx::PngImage drawn = Rtx::readPng(frameFile(wrote, view.mName));
            const Rtx::PngImage reference = Rtx::readPng(frameFile(against, view.mName));
            const FrameDifference difference = compareFrames(reference, drawn);

            out() << std::format("  {:<28} {}\n", view.mName, describe(difference));

            if (difference.mMismatched)
                ++unmatched;
            else if (!difference.same())
                ++differing;
        }

        if (differing == 0 && unmatched == 0)
        {
            out() << "  every view is the same picture\n";
            return 0;
        }

        out() << std::format(
            "  {} of {} views moved, {} had nothing to compare against\n", differing, views.size(), unmatched);

        return 1;
    }
}
