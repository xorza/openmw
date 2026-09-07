#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
    /// repaired in different places. `Rtx::digestLayout` is the second hash and answers which:
    /// `island-crossing` differed on 37 frames of 360 while it differed on all 360, which named the
    /// world rather than the renderer and was what the report could not say before.
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
        /// @param scene what `Rtx::digestLayout` made of the description that drew them.
        void add(std::string_view view, std::uint32_t frame, std::span<const std::uint8_t> pixels,
            const std::array<std::uint64_t, 2>& scene);

        void write(const std::filesystem::path& file) const;

        std::size_t frameCount() const { return mFrames.size(); }

        /// What one view came to against `reference`.
        struct ViewDifference
        {
            std::string mView;
            std::uint32_t mFrames = 0;

            /// Frames whose picture differs, in order.
            std::vector<std::uint32_t> mDiffering;

            /// Frames whose scene differs, in order.
            ///
            /// **Reported and not judged.** Two builds are expected to lay a scene out differently
            /// and to draw the same picture from it, so a difference here is what a reader needs to
            /// know and never what fails a run — `same` says so by leaving it out.
            std::vector<std::uint32_t> mSceneDiffering;

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
            std::array<std::uint64_t, 2> mScene{};
        };

        std::vector<Frame> mFrames;
    };

    /// The differing frames of one view, as a line for the report — or empty where it matched.
    std::string describeDifference(const FrameHashes::ViewDifference& difference);
}
