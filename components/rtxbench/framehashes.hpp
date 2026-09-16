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
    /// Two hashes a frame of a run — what it drew and what it was handed — and what a previous
    /// run's hashes say about this one: `shot --against` for a run rather than a still, because a stale
    /// table or a history reprojected onto the wrong surface needs a second frame to show. The
    /// scene beside the picture, because a run that differs has either drawn one scene two ways or
    /// been handed two scenes, and those are repaired in different places; a column per part,
    /// because "the layout moved" names no table. A hash and not a picture, because six hundred
    /// frames is a few hundred megabytes; a frame it names is then rendered on its own for a look.
    class FrameHashes
    {
    public:
        /// Reads what a previous run wrote. Throws `Rtx::Error` where the file will not parse, so a
        /// truncated reference is a failure and not a run that silently matches nothing.
        static FrameHashes read(const std::filesystem::path& file);

        /// A frame drawn: `parts` is what `SceneDigester::digest` made of the description that
        /// drew it, known as it is drawn, and `submitted` is what `Renderer::getFrameCount`
        /// numbered it, which is how `picture` finds the row once the picture has come back.
        void note(std::string_view view, std::uint32_t frame, std::uint64_t submitted, const ScenePartDigests& parts);

        /// The picture of the frame numbered `submitted`, once it has come back — `pixels` as the
        /// tool would write them to a PNG, so a hash names the picture a person would look at.
        /// Nothing for a frame nobody noted, which a warm-up's is.
        void picture(std::uint64_t submitted, std::span<const std::uint8_t> pixels);

        /// How many rows are noted and not yet pictured: what a stop that did not drain its ring
        /// leaves, and what `write` refuses to write.
        std::size_t countUnpictured() const;

        /// Throws where a row has no picture: a file with a hash of nothing in it would compare
        /// as a frame that moved, and the ring is what was not drained.
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
            std::vector<std::uint32_t> mSceneDiffering;

            /// How many frames each part differs on, indexed by `ScenePart`.
            std::array<std::uint32_t, static_cast<std::size_t>(ScenePart::Count)> mPartsDiffering{};

            /// Frames this run drew that the reference has no hash for, and the other way about.
            std::uint32_t mUnmatched = 0;

            /// Whether nothing at all moved: not a picture, not a part of the scene, not the count
            /// of frames. The scene as well as the picture, because a run of one binary repeats
            /// every column exactly and a picture the same over a scene that moved is a world
            /// handed over twice, which the report names the part of.
            bool same() const { return mDiffering.empty() && mSceneDiffering.empty() && mUnmatched == 0; }
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

            /// The renderer's own number for the frame, for `picture` alone; not written.
            std::uint64_t mSubmitted = 0;
            bool mPictured = false;
        };

        std::vector<Frame> mFrames;
    };

    /// The differing frames of one view, as a line for the report — or empty where it matched.
    std::string describeDifference(const FrameHashes::ViewDifference& difference);
}
