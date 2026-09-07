#include "framehashes.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <sstream>
#include <utility>

#include <smhasher/MurmurHash3.h>

#include <components/files/conversion.hpp>
#include <components/rtx/error.hpp>

namespace Rtx
{
    namespace
    {
        /// How many differing frames a report names before it stops counting them out.
        constexpr std::size_t sNamed = 6;
    }

    void Digest::add(std::span<const std::byte> bytes)
    {
        // The seed is read whole before anything is written, but a copy costs two words and makes
        // that true whatever the implementation does.
        const std::array<std::uint64_t, 2> seed = mWords;
        MurmurHash3_x64_128(bytes.data(), static_cast<int>(bytes.size()), seed.data(), mWords.data());
    }

    std::string spellHash(const std::array<std::uint64_t, 2>& words)
    {
        return std::format("{:016x}{:016x}", words[0], words[1]);
    }

    void FrameHashes::add(const std::string_view view, const std::uint32_t frame, std::span<const std::uint8_t> pixels,
        const std::array<std::uint64_t, 2>& scene)
    {
        Digest digest;
        digest.add(pixels);
        mFrames.push_back(
            Frame{ .mView = std::string(view), .mFrame = frame, .mHash = digest.getWords(), .mScene = scene });
    }

    void FrameHashes::write(const std::filesystem::path& file) const
    {
        std::ofstream out(file);
        for (const Frame& held : mFrames)
            out << std::format("{} {} {} {}\n", held.mView, held.mFrame, spellHash(held.mHash), spellHash(held.mScene));

        // **Thrown and not reported**, the way `shot --dump` answers the same failure: a reference
        // that did not get written and a command that still succeeded is the next run comparing
        // against whatever was at that path before.
        if (!out)
            throw Error("could not write " + Files::pathToUnicodeString(file));
    }

    FrameHashes FrameHashes::read(const std::filesystem::path& file)
    {
        std::ifstream in(file);
        if (!in)
            throw Error("could not read " + Files::pathToUnicodeString(file));

        FrameHashes held;
        std::string line;
        while (std::getline(in, line))
        {
            if (line.empty())
                continue;

            std::istringstream fields(line);
            Frame frame;
            std::string picture;
            std::string scene;
            fields >> frame.mView >> frame.mFrame >> picture >> scene;

            // **Every line or none.** A reference read half way is one that matches the frames it
            // reached and says nothing about the rest, which reads as a pass. A file one column
            // short is a run of a build from before the scene was hashed, and it fails here rather
            // than comparing against a column nobody wrote.
            if (!fields || picture.size() != 32 || scene.size() != 32)
                throw Error("cannot read " + Files::pathToUnicodeString(file) + ": " + line);

            for (int half = 0; half < 2; ++half)
            {
                const char* from = picture.data() + half * 16;
                if (std::from_chars(from, from + 16, frame.mHash[half], 16).ec != std::errc{})
                    throw Error("cannot read " + Files::pathToUnicodeString(file) + ": " + line);

                from = scene.data() + half * 16;
                if (std::from_chars(from, from + 16, frame.mScene[half], 16).ec != std::errc{})
                    throw Error("cannot read " + Files::pathToUnicodeString(file) + ": " + line);
            }

            held.mFrames.push_back(std::move(frame));
        }

        return held;
    }

    std::vector<FrameHashes::ViewDifference> FrameHashes::against(const FrameHashes& reference) const
    {
        std::vector<ViewDifference> differences;

        for (const Frame& held : mFrames)
        {
            if (differences.empty() || differences.back().mView != held.mView)
                differences.push_back(ViewDifference{ .mView = held.mView });

            ViewDifference& difference = differences.back();
            ++difference.mFrames;

            const auto found = std::find_if(reference.mFrames.begin(), reference.mFrames.end(),
                [&](const Frame& was) { return was.mFrame == held.mFrame && was.mView == held.mView; });

            if (found == reference.mFrames.end())
            {
                ++difference.mUnmatched;
                continue;
            }

            if (found->mHash != held.mHash)
                difference.mDiffering.push_back(held.mFrame);
            if (found->mScene != held.mScene)
                difference.mSceneDiffering.push_back(held.mFrame);
        }

        // **What the reference drew and this run did not**, which is a schedule that changed rather
        // than a picture that did: a run of fewer frames matches every frame it drew.
        for (ViewDifference& difference : differences)
        {
            const auto missing = std::count_if(reference.mFrames.begin(), reference.mFrames.end(),
                [&](const Frame& was) { return was.mView == difference.mView; });

            if (static_cast<std::uint32_t>(missing) > difference.mFrames)
                difference.mUnmatched += static_cast<std::uint32_t>(missing) - difference.mFrames;
        }

        return differences;
    }

    std::string describeDifference(const FrameHashes::ViewDifference& difference)
    {
        // **The scene is asked here too, though it does not fail the run.** Reporting only the
        // picture is what let a run be called identical while the description behind it moved on
        // every frame, which is the fault this column was added for.
        if (difference.same() && difference.mSceneDiffering.empty())
            return std::format("{} frames, every one of them the same", difference.mFrames);

        std::string report;
        if (!difference.mDiffering.empty())
        {
            report = std::format("{} of {} frames differ, at ", difference.mDiffering.size(), difference.mFrames);
            for (std::size_t at = 0; at < std::min(sNamed, difference.mDiffering.size()); ++at)
                report += (at > 0 ? ", " : "") + std::to_string(difference.mDiffering[at]);

            if (difference.mDiffering.size() > sNamed)
                report += std::format(" and {} more", difference.mDiffering.size() - sNamed);
        }
        else if (!difference.mSceneDiffering.empty())
            report = std::format("{} frames, every picture the same", difference.mFrames);

        // **Which of the two moved, which is what says where to look next.** A picture that differs
        // where the scene differs is a world handed over twice, and belongs to whatever staged it.
        // One that differs where the scene did not is the renderer under it.
        if (!difference.mSceneDiffering.empty())
        {
            report += std::format("; the scene differs on {} frames", difference.mSceneDiffering.size());

            if (!difference.mDiffering.empty())
            {
                const auto both = std::count_if(
                    difference.mDiffering.begin(), difference.mDiffering.end(), [&](const std::uint32_t frame) {
                        return std::binary_search(
                            difference.mSceneDiffering.begin(), difference.mSceneDiffering.end(), frame);
                    });

                report += std::format(", {} of them among those", both);
            }
        }
        else if (!difference.mDiffering.empty())
            report += "; the scene was the same on every frame";

        if (difference.mUnmatched > 0)
            report += std::format(
                "{}{} frames the two runs do not share", report.empty() ? "" : "; ", difference.mUnmatched);

        return report;
    }
}
