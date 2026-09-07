#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "scenedigest.hpp"

namespace Rtx
{
    /// MurmurHash3 over whatever is fed to it, in the order it is fed.
    ///
    /// **Chained through the seed, which is why this fork's seed is two words**: each span is hashed
    /// with the result so far as the seed, so a digest of many spans needs no copy of them laid end
    /// to end. `FrameHashes` feeds it one frame's pixels; `scene` feeds it a whole description, a
    /// field at a time.
    class Digest
    {
    public:
        void add(std::span<const std::byte> bytes);

        template <class T>
        void add(std::span<const T> values)
        {
            add(std::as_bytes(values));
        }

        template <class T>
        void add(const T& value)
        {
            add(std::span<const T>(&value, 1));
        }

        const std::array<std::uint64_t, 2>& getWords() const { return mWords; }

    private:
        std::array<std::uint64_t, 2> mWords{};
    };

    /// Thirty-two hex digits, which is how a hashes file spells one and how `scene` reports one.
    std::string spellHash(const std::array<std::uint64_t, 2>& words);

    /// Two hashes a frame of a run — what it drew and what it was handed — and what a previous
    /// run's hashes say about this one.
    ///
    /// **`verify` for a run rather than a view.** `verify` renders sixteen standing views and
    /// compares every pixel against a stored reference, which settles anything a single frame can
    /// show. It shows nothing that needs a second frame — a table copy a placement missed, a history
    /// reprojected onto the wrong surface, a row two frames stale — and those are where the defects
    /// are. Judging a moving run by a summary of a frame instead is judging it by a number with no
    /// expected value, which cannot tell a stale table from a camera that moved.
    ///
    /// **The scene beside the picture, because a picture that moved says nothing about why.** A run
    /// that differs has either drawn one scene two ways or been handed two scenes, and those are
    /// repaired in different places. `island-crossing` differed on 37 frames of 360 while what it
    /// was handed differed on all 360, which named the world rather than the renderer and was what
    /// the report could not say before.
    ///
    /// **A column per part of the scene and not one number for the lot, because "the layout moved"
    /// names no table.** `Rtx::ScenePart` says why. What a file holds is a header row and then a row
    /// a frame, so two runs are compared column by column and a report says which of them moved.
    ///
    /// **The parts and nothing dearer than they are.** `Rtx::digestScene` answers the other
    /// question — whether the two runs held one world or two — and it costs a walk of every triangle
    /// of every placement: measured on `island-crossing`, a hashed run of 360 frames took 122
    /// seconds with it against 8.6 without. `scene` and `verify` ask it once at a place, which is
    /// where it is affordable.
    ///
    /// **A hash and not a picture**, because six hundred frames at 1920x1080 is a few hundred
    /// megabytes and the sixteen stills are kilobytes. What this answers is "did the run draw the
    /// same frames", and it names the ones that changed; what it cannot answer is by how much, and
    /// a frame it names is then rendered on its own for a look.
    class FrameHashes
    {
    public:
        /// Reads what a previous run wrote. Throws `Rtx::Error` where the file will not parse, so a
        /// reference that was truncated is a failure and not a run that silently matches nothing.
        static FrameHashes read(const std::filesystem::path& file);

        /// @param pixels **as the tool would write them to a PNG**, so a hash names the picture a
        ///        person would look at rather than an internal channel that may not survive a
        ///        rebuild.
        /// @param parts what `Rtx::digestParts` made of the description that drew them, one column
        ///        each.
        void add(std::string_view view, std::uint32_t frame, std::span<const std::uint8_t> pixels,
            const ScenePartDigests& parts);

        void write(const std::filesystem::path& file) const;

        std::size_t frameCount() const { return mFrames.size(); }

        /// What one view came to against `reference`.
        struct ViewDifference
        {
            std::string mView;
            std::uint32_t mFrames = 0;

            /// Frames whose picture differs, in order.
            std::vector<std::uint32_t> mDiffering;

            /// Frames where any part of the scene differs, in order.
            ///
            /// **Reported and not judged.** Two builds are expected to lay a scene out differently
            /// and to draw the same picture from it, so a difference here is what a reader needs to
            /// know and never what fails a run — `same` says so by leaving it out.
            std::vector<std::uint32_t> mSceneDiffering;

            /// How many frames each part differs on, indexed by `ScenePart`.
            std::array<std::uint32_t, static_cast<std::size_t>(ScenePart::Count)> mPartsDiffering{};

            /// Frames this run drew that the reference has no hash for, and the other way about.
            std::uint32_t mUnmatched = 0;

            bool same() const { return mDiffering.empty() && mUnmatched == 0; }
        };

        /// One entry per view this run drew, in the order it drew them.
        std::vector<ViewDifference> against(const FrameHashes& reference) const;

    private:
        struct Frame
        {
            std::string mView;
            std::uint32_t mFrame = 0;
            std::array<std::uint64_t, 2> mHash{};
            ScenePartDigests mParts{};
        };

        std::vector<Frame> mFrames;
    };

    /// The differing frames of one view, as a line for the report — or empty where it matched.
    std::string describeDifference(const FrameHashes::ViewDifference& difference);
}
