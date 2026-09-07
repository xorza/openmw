#include "framehashes.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
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

        /// The columns before the parts: the view, the frame and the picture.
        constexpr std::size_t sNamedColumns = 3;

        constexpr std::size_t sColumns = sNamedColumns + static_cast<std::size_t>(ScenePart::Count);

        /// What a file opens with, and the one statement of what its columns are.
        std::string headerLine()
        {
            std::string header = "view,frame,picture";
            for (std::size_t part = 0; part < static_cast<std::size_t>(ScenePart::Count); ++part)
                header += ',' + std::string(nameOf(static_cast<ScenePart>(part)));

            return header;
        }

        /// Which parts moved and on how many frames, biggest first — or nothing where none did.
        std::string namePartsDiffering(const FrameHashes::ViewDifference& difference)
        {
            std::vector<std::size_t> moved;
            for (std::size_t part = 0; part < difference.mPartsDiffering.size(); ++part)
                if (difference.mPartsDiffering[part] > 0)
                    moved.push_back(part);

            if (moved.empty())
                return {};

            // Stable, so that parts that moved on as many frames read in the order a scene is laid
            // out rather than in whichever order the sort left them.
            std::stable_sort(moved.begin(), moved.end(), [&](const std::size_t left, const std::size_t right) {
                return difference.mPartsDiffering[left] > difference.mPartsDiffering[right];
            });

            std::string named = " — ";
            for (std::size_t at = 0; at < moved.size(); ++at)
                named += std::format("{}{} {}", at > 0 ? ", " : "", nameOf(static_cast<ScenePart>(moved[at])),
                    difference.mPartsDiffering[moved[at]]);

            return named;
        }
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
        const ScenePartDigests& parts)
    {
        Digest digest;
        digest.add(pixels);
        mFrames.push_back(
            Frame{ .mView = std::string(view), .mFrame = frame, .mHash = digest.getWords(), .mParts = parts });
    }

    void FrameHashes::write(const std::filesystem::path& file) const
    {
        std::ofstream out(file);
        out << headerLine() << '\n';

        for (const Frame& held : mFrames)
        {
            out << held.mView << ',' << held.mFrame << ',' << spellHash(held.mHash);
            for (const std::array<std::uint64_t, 2>& part : held.mParts)
                out << ',' << spellHash(part);

            out << '\n';
        }

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

        const auto fail = [&](const std::string& line) {
            return Error("cannot read " + Files::pathToUnicodeString(file) + ": " + line);
        };

        std::string line;

        // **The header has to be this build's, exactly.** A file written before a column existed
        // would otherwise be compared column by column against one that has it, and every row would
        // read as a difference in a table nobody changed.
        if (!std::getline(in, line) || line != headerLine())
            throw fail(line);

        const auto readHash = [](const std::string_view field, std::array<std::uint64_t, 2>& into) {
            if (field.size() != 32)
                return false;

            for (int half = 0; half < 2; ++half)
            {
                const char* const from = field.data() + half * 16;
                if (std::from_chars(from, from + 16, into[half], 16).ec != std::errc{})
                    return false;
            }

            return true;
        };

        // Cleared and refilled a line at a time, rather than allocated per line of a file a run
        // reads in full.
        std::vector<std::string_view> fields;

        FrameHashes held;
        while (std::getline(in, line))
        {
            if (line.empty())
                continue;

            fields.clear();
            for (std::size_t at = 0; at <= line.size();)
            {
                const std::size_t comma = std::min(line.find(',', at), line.size());
                fields.push_back(std::string_view(line).substr(at, comma - at));
                at = comma + 1;
            }

            // **Every line or none.** A reference read half way is one that matches the frames it
            // reached and says nothing about the rest, which reads as a pass.
            if (fields.size() != sColumns)
                throw fail(line);

            Frame frame;
            frame.mView = std::string(fields[0]);
            if (std::from_chars(fields[1].data(), fields[1].data() + fields[1].size(), frame.mFrame).ec != std::errc{})
                throw fail(line);

            if (!readHash(fields[2], frame.mHash))
                throw fail(line);

            for (std::size_t part = 0; part < frame.mParts.size(); ++part)
                if (!readHash(fields[sNamedColumns + part], frame.mParts[part]))
                    throw fail(line);

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

            bool anyPart = false;
            for (std::size_t part = 0; part < held.mParts.size(); ++part)
            {
                if (found->mParts[part] == held.mParts[part])
                    continue;

                ++difference.mPartsDiffering[part];
                anyPart = true;
            }

            if (anyPart)
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
        // every frame, which is the fault these columns were added for.
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

            // Last, because it is a list and anything appended after it would read as part of it.
            report += namePartsDiffering(difference);
        }
        else if (!difference.mDiffering.empty())
            report += "; the scene was the same on every frame";

        if (difference.mUnmatched > 0)
            report += std::format(
                "{}{} frames the two runs do not share", report.empty() ? "" : "; ", difference.mUnmatched);

        return report;
    }
}
