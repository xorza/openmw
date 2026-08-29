#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace RtxTool
{
    /// One hash a frame of a run, and what a previous run's hashes say about this one.
    ///
    /// **`verify` for a run rather than a view.** `verify` renders sixteen standing views and
    /// compares every pixel against a stored reference, which settles anything a single frame can
    /// show. It shows nothing that needs a second frame — a table copy a placement missed, a history
    /// reprojected onto the wrong surface, a row two frames stale — and those are where the defects
    /// are. Judging a moving run by a summary of a frame instead is judging it by a number with no
    /// expected value, which cannot tell a stale table from a camera that moved.
    ///
    /// **A run stays comparable with itself only while nothing in it reads the wall clock.**
    /// `Rtx::FrameOptions::mSinceLast` and `Rtx::SceneUploader::setSettled` are the two that did.
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

        /// **The pixels as the tool would write them to a PNG**, so a hash names the picture a
        /// person would look at rather than an internal channel that may not survive a rebuild.
        void add(std::string_view view, std::uint32_t frame, std::span<const std::uint8_t> pixels);

        void write(const std::filesystem::path& file) const;

        std::size_t frameCount() const { return mFrames.size(); }

        /// What one view came to against `reference`.
        struct ViewDifference
        {
            std::string mView;
            std::uint32_t mFrames = 0;

            /// Frames whose hash differs, in order.
            std::vector<std::uint32_t> mDiffering;

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
        };

        std::vector<Frame> mFrames;
    };

    /// The differing frames of one view, as a line for the report — or empty where it matched.
    std::string describe(const FrameHashes::ViewDifference& difference);
}
