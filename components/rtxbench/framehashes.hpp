#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <components/rtx/framedigest.hpp>
#include <components/rtx/frameimage.hpp>
#include <components/rtx/shaders/digest.h>
#include <components/rtx/upscale.hpp>

#include "scenedigest.hpp"

namespace Rtx
{
    struct FrameResult;

    /// Which digested column of a frame moved: every channel of the trace at its binding, the
    /// composite after them, and last the numbers the frame handed the reconstruction.
    inline constexpr std::size_t sTracedColumns = Shaders::DIGEST_IMAGES + 1;

    inline constexpr std::size_t sReconstructionColumn = Shaders::DIGEST_IMAGES;

    constexpr std::string_view tracedName(const std::size_t column)
    {
        if (column < sChannelCount)
            return channelName(static_cast<Channel>(column));

        return column == Shaders::DIGEST_COMPOSITE ? "composite" : "reconstruction";
    }

    /// What a frame of a run computed and what it was handed, hashed, and what a previous run's
    /// hashes say about this one: `shot --against` for a run rather than a still, because a stale
    /// table or a history reprojected onto the wrong surface needs a second frame to show.
    ///
    /// **Three things a frame, and the verdict is never the picture past a network.** The trace's
    /// own images and what the frame handed the reconstruction are what the tree computed, and two
    /// runs of one build compute them the same to the bit. The picture is the same only where
    /// nothing reconstructed it: a network keeps a history, and the smallest difference in what it
    /// was handed on one frame stays in its picture for the rest of the run, so its picture is
    /// written for a look and compared for the record, and a run under it is the same run where
    /// the trace and the scene are. The scene beside both, because a run that differs has either
    /// drawn one scene two ways or been handed two scenes, and those are repaired in different
    /// places; a column per part, because "the layout moved" names no table. A hash and not a
    /// picture, because six hundred frames is a few hundred megabytes; a frame it names is then
    /// rendered on its own for a look.
    class FrameHashes
    {
    public:
        /// Reads what a previous run wrote. Throws `Rtx::InputError` where the file will not parse,
        /// so a truncated reference is a failure and not a run that silently matches nothing.
        static FrameHashes read(const std::filesystem::path& file);

        /// A frame drawn: `parts` is what `SceneDigester::digest` made of the description that
        /// drew it, known as it is drawn, and `submitted` is what `Renderer::getFrameCount`
        /// numbered it, which is how `picture` finds the row once the picture has come back.
        void note(std::string_view view, std::uint32_t frame, std::uint64_t submitted, const ScenePartDigests& parts);

        /// Which row a picture landed on: the view and the frame the hashes file spells it as.
        /// The view is the row's own, and stands until the next `note`.
        struct Pictured
        {
            std::string_view mView;
            std::uint32_t mFrame = 0;
        };

        /// The frame numbered `finished.mFrame`, once it has come back with its picture, its
        /// digest and what reconstructed it. Nothing for a frame nobody noted, which a warm-up's
        /// is, and the row it landed on otherwise, so a caller keeping the picture itself can
        /// file it under the frame the report will name.
        std::optional<Pictured> picture(const FrameResult& finished);

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

            /// Frames where any traced column differs, in order: the renderer drew one scene two
            /// ways, or handed the reconstruction something else.
            std::vector<std::uint32_t> mTraceDiffering;

            /// How many frames each traced column differs on, indexed as `tracedName`.
            std::array<std::uint32_t, sTracedColumns> mTracedDiffering{};

            /// Frames whose picture differs where nothing reconstructed it, in order.
            std::vector<std::uint32_t> mDiffering;

            /// Frames whose picture differs past a network, in order. Reported and never a verdict.
            std::vector<std::uint32_t> mReconstructedDiffering;

            /// Frames where any part of the scene differs, in order.
            std::vector<std::uint32_t> mSceneDiffering;

            /// How many frames each part differs on, indexed by `ScenePart`.
            std::array<std::uint32_t, static_cast<std::size_t>(ScenePart::Count)> mPartsDiffering{};

            /// Frames the two runs reconstructed differently — one upscaled and the other not, or
            /// at another quality — which is two configurations and not one run twice.
            std::uint32_t mUpscaledDiffering = 0;

            /// Frames this run drew that the reference has no hash for, and the other way about.
            std::uint32_t mUnmatched = 0;

            /// Whether nothing the tree computed moved: not the trace, not a picture it drew
            /// itself, not a part of the scene, not the configuration, not the count of frames.
            /// The scene as well as the trace, because a run of one binary repeats every column
            /// exactly and a trace the same over a scene that moved is a world handed over twice,
            /// which the report names the part of.
            bool same() const
            {
                return mTraceDiffering.empty() && mDiffering.empty() && mSceneDiffering.empty()
                    && mUpscaledDiffering == 0 && mUnmatched == 0;
            }
        };

        /// One entry per view this run drew, in the order it drew them.
        std::vector<ViewDifference> against(const FrameHashes& reference) const;

        /// The first frame of `view` whose depth or motion differs from the view's first frame, or
        /// nothing where every frame pictured so far agrees on both. For a still nothing jittered,
        /// whose frames are one camera's: its depth and motion are then one frame's whatever the
        /// noise did to the light, and a frame where either moved was traced on other code: the
        /// driver swapping in one that computes an operation the build leaves to the device
        /// otherwise (`Rtx::pinFloatArithmetic`).
        std::optional<std::uint32_t> findStillMoved(std::string_view view) const;

    private:
        struct Frame
        {
            std::string mView;
            std::uint32_t mFrame = 0;

            /// What reconstructed the picture, so a comparison knows whose it is.
            Upscale mUpscale = Upscale::Off;

            DigestWords mHash{};
            std::array<DigestWords, sTracedColumns> mTraced{};
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
