#include "framehashes.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <components/files/conversion.hpp>
#include <components/rtx/error.hpp>
#include <components/rtx/renderer.hpp>

namespace Rtx
{
    namespace
    {
        /// How many differing frames a report names before it stops counting them out.
        constexpr std::size_t sNamed = 6;

        /// The columns before the digests: the view, the frame, what reconstructed the picture,
        /// and the picture.
        constexpr std::size_t sNamedColumns = 4;

        constexpr std::size_t sColumns = sNamedColumns + sTracedColumns + static_cast<std::size_t>(ScenePart::Count);

        /// What a file opens with, and the one statement of what its columns are.
        std::string headerLine()
        {
            // The format's own number first, stepped where the columns keep their names and a
            // value changes its meaning — a digest hashed another way — so a file an older build
            // wrote is refused rather than compared.
            std::string header = "hashes 4: view,frame,upscale,picture";
            for (std::size_t column = 0; column < sTracedColumns; ++column)
                header += ',' + std::string(tracedName(column));
            for (const auto& [part, name] : sSceneParts.mNames)
                header += ',' + std::string(name);

            return header;
        }

        /// The first few of `frames` spelled out, and how many more there are.
        std::string nameFrames(const std::vector<std::uint32_t>& frames)
        {
            std::string named;
            for (std::size_t at = 0; at < std::min(sNamed, frames.size()); ++at)
                named += (at > 0 ? ", " : "") + std::to_string(frames[at]);

            if (frames.size() > sNamed)
                named += std::format(" and {} more", frames.size() - sNamed);

            return named;
        }

        /// Which columns moved and on how many frames, biggest first — or nothing where none did.
        template <std::size_t Count, class NameOf>
        std::string nameColumnsDiffering(const std::array<std::uint32_t, Count>& counts, const NameOf& nameOf)
        {
            std::vector<std::size_t> moved;
            for (std::size_t column = 0; column < counts.size(); ++column)
                if (counts[column] > 0)
                    moved.push_back(column);

            if (moved.empty())
                return {};

            // Stable, so that columns that moved on as many frames read in the order the table
            // lays them out rather than in whichever order the sort left them.
            std::stable_sort(moved.begin(), moved.end(),
                [&](const std::size_t left, const std::size_t right) { return counts[left] > counts[right]; });

            std::string named = " — ";
            for (std::size_t at = 0; at < moved.size(); ++at)
                named += std::format("{}{} {}", at > 0 ? ", " : "", nameOf(moved[at]), counts[moved[at]]);

            return named;
        }

        std::string_view partName(const std::size_t part)
        {
            return nameOf(static_cast<ScenePart>(part));
        }

        /// The numbers the frame handed the reconstruction, as one column: what a wrong sign on
        /// the jitter or a reset that never clears would move, and nothing in an image would.
        DigestWords digestHanded(const FrameDigest& digest)
        {
            Digest words;
            words.add(digest.mJitterX);
            words.add(digest.mJitterY);
            words.add(digest.mFrameDeltaMs);
            words.add(digest.mReset);
            return words.getWords();
        }
    }

    void FrameHashes::note(const std::string_view view, const std::uint32_t frame, const std::uint64_t submitted,
        const ScenePartDigests& parts)
    {
        mFrames.push_back(
            Frame{ .mView = std::string(view), .mFrame = frame, .mParts = parts, .mSubmitted = submitted });
    }

    std::optional<FrameHashes::Pictured> FrameHashes::picture(const FrameResult& finished)
    {
        // From the back, because the frame that came back is one of the last few noted.
        const auto row = std::find_if(mFrames.rbegin(), mFrames.rend(),
            [&](const Frame& held) { return held.mSubmitted == finished.mFrame && !held.mPictured; });
        if (row == mFrames.rend())
            return std::nullopt;

        assert(finished.mDigest.has_value() && "a frame read back without the digest the same option asks for");

        Digest digest;
        digest.add(finished.mPixels);
        row->mHash = digest.getWords();

        for (std::size_t image = 0; image < Shaders::DIGEST_IMAGES; ++image)
            row->mTraced[image] = finished.mDigest->mImages[image];
        row->mTraced[sReconstructionColumn] = digestHanded(*finished.mDigest);

        row->mUpscale = finished.mReconstruction.mUpscaling.mMode;
        row->mPictured = true;

        return Pictured{ .mView = row->mView, .mFrame = row->mFrame };
    }

    std::size_t FrameHashes::countUnpictured() const
    {
        return static_cast<std::size_t>(
            std::count_if(mFrames.begin(), mFrames.end(), [](const Frame& held) { return !held.mPictured; }));
    }

    void FrameHashes::write(const std::filesystem::path& file) const
    {
        if (const std::size_t unpictured = countUnpictured(); unpictured > 0)
            throw Error(std::to_string(unpictured) + " frames were noted and never pictured; the ring was not drained");

        std::ofstream out(file);
        out << headerLine() << '\n';

        for (const Frame& held : mFrames)
        {
            out << held.mView << ',' << held.mFrame << ',' << sUpscaleNames.name(held.mUpscale) << ','
                << spellHash(held.mHash);
            for (const DigestWords& column : held.mTraced)
                out << ',' << spellHash(column);
            for (const DigestWords& part : held.mParts)
                out << ',' << spellHash(part);

            out << '\n';
        }

        // **Thrown and not reported**: a reference that did not get written and a command that
        // still succeeded is the next run comparing against whatever was at that path before.
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

        const auto readHash = [](const std::string_view field, DigestWords& into) {
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

            const std::optional<Upscale> upscale = sUpscaleNames.named(fields[2]);
            if (!upscale.has_value())
                throw fail(line);
            frame.mUpscale = *upscale;

            if (!readHash(fields[3], frame.mHash))
                throw fail(line);
            frame.mPictured = true;

            for (std::size_t column = 0; column < sTracedColumns; ++column)
                if (!readHash(fields[sNamedColumns + column], frame.mTraced[column]))
                    throw fail(line);

            for (std::size_t part = 0; part < frame.mParts.size(); ++part)
                if (!readHash(fields[sNamedColumns + sTracedColumns + part], frame.mParts[part]))
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

            if (found->mUpscale != held.mUpscale)
                ++difference.mUpscaledDiffering;

            bool anyTraced = false;
            for (std::size_t column = 0; column < sTracedColumns; ++column)
            {
                if (found->mTraced[column] == held.mTraced[column])
                    continue;

                ++difference.mTracedDiffering[column];
                anyTraced = true;
            }

            if (anyTraced)
                difference.mTraceDiffering.push_back(held.mFrame);

            // **Whose picture it is decides which list it goes on.** Where either run put a network
            // between the trace and the picture, the picture is the network's, and two runs of
            // one build are allowed to disagree about it.
            if (found->mHash != held.mHash)
            {
                if (found->mUpscale == Upscale::Off && held.mUpscale == Upscale::Off)
                    difference.mDiffering.push_back(held.mFrame);
                else
                    difference.mReconstructedDiffering.push_back(held.mFrame);
            }

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
        if (difference.same())
        {
            if (difference.mReconstructedDiffering.empty())
                return std::format("{} frames, every one of them the same", difference.mFrames);

            return std::format(
                "{} frames, the trace and the scene the same on every one; the reconstructed picture "
                "differs on {}, which is the network's and not a verdict",
                difference.mFrames, difference.mReconstructedDiffering.size());
        }

        std::vector<std::string> clauses;

        // **The trace first, because it is the verdict.** A picture that differs where the trace
        // differs is the same finding twice; one that differs where the trace did not is the
        // display chain, or the network past it.
        if (!difference.mTraceDiffering.empty())
            clauses.push_back(std::format("the trace differs on {} of {} frames, at {}{}",
                difference.mTraceDiffering.size(), difference.mFrames, nameFrames(difference.mTraceDiffering),
                nameColumnsDiffering(difference.mTracedDiffering, tracedName)));

        if (!difference.mDiffering.empty())
            clauses.push_back(std::format("the picture differs on {} of {} frames, at {}", difference.mDiffering.size(),
                difference.mFrames, nameFrames(difference.mDiffering)));

        if (!difference.mReconstructedDiffering.empty())
            clauses.push_back(
                std::format("the reconstructed picture differs on {} frames, which is the network's and "
                            "not a verdict",
                    difference.mReconstructedDiffering.size()));

        // **Which of the two moved, which is what says where to look next.** A trace that differs
        // where the scene differs is a world handed over twice, and belongs to whatever staged it.
        // One that differs where the scene did not is the renderer under it.
        if (!difference.mSceneDiffering.empty())
        {
            std::string scene = std::format("the scene differs on {} frames", difference.mSceneDiffering.size());

            if (!difference.mTraceDiffering.empty())
            {
                const auto both = std::count_if(difference.mTraceDiffering.begin(), difference.mTraceDiffering.end(),
                    [&](const std::uint32_t frame) {
                        return std::binary_search(
                            difference.mSceneDiffering.begin(), difference.mSceneDiffering.end(), frame);
                    });

                scene += std::format(", {} of them among those the trace differs on", both);
            }

            // Last, because it is a list and anything appended after it would read as part of it.
            clauses.push_back(scene + nameColumnsDiffering(difference.mPartsDiffering, partName));
        }
        else if (!difference.mTraceDiffering.empty() || !difference.mDiffering.empty())
            clauses.push_back("the scene was the same on every frame");

        if (difference.mUpscaledDiffering > 0)
            clauses.push_back(
                std::format("the two runs reconstructed {} frames differently", difference.mUpscaledDiffering));

        if (difference.mUnmatched > 0)
            clauses.push_back(std::format("{} frames the two runs do not share", difference.mUnmatched));

        std::string report;
        for (std::size_t at = 0; at < clauses.size(); ++at)
            report += (at > 0 ? "; " : "") + clauses[at];

        return report;
    }
}
