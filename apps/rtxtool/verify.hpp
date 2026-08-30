#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include <components/rtx/png.hpp>

#include "framerequest.hpp"
#include "views.hpp"

namespace Rtx
{
    struct ValidationOptions;
}

namespace RtxTool
{
    class World;

    /// An A/B of the picture, over every view there is.
    ///
    /// **What this is for is saying whether a change moved the picture, and by how much.** A
    /// refactor of the geometry path is supposed to leave every frame exactly as it was, and the
    /// only thing that can say so is the previous build's own frames — so the reference is a
    /// directory this command wrote earlier on this machine, never a corpus in the tree. The picture
    /// is a function of the driver and the card as much as of the code, and checked-in bytes would
    /// be a promise the tree cannot keep.
    struct VerifyRequest
    {
        FrameRequest mFrame;

        /// The places to render, in the order they are rendered.
        std::vector<View> mViews;

        /// Where this run's frames go. Created if it is not there.
        std::filesystem::path mOut;

        /// A previous run's directory, or empty to render a reference and compare nothing.
        std::filesystem::path mAgainst;
    };

    /// What two renderings of one view came to.
    ///
    /// **A magnitude and not a verdict.** "Worst 2 of 255 on 5% of the pixels" is a rounding
    /// difference and "worst 37 on 20%" is a bug, and a bare *differs* — which is all this fork had
    /// — cost a day of bisection that reached the wrong answer twice.
    struct FrameDifference
    {
        /// The two are not the same size, so there is nothing to subtract. Also what a missing or
        /// unreadable reference reads as.
        bool mMismatched = false;

        /// How many pixels differ in any colour channel, out of how many there are.
        ///
        /// **Colour only.** Alpha out of the tone curve says nothing about what the frame looks
        /// like, and a difference confined to it is not one anybody can see.
        std::uint64_t mDiffering = 0;
        std::uint64_t mTotal = 0;

        /// The largest difference any one channel showed, out of 255.
        std::uint32_t mWorst = 0;

        bool same() const { return !mMismatched && mDiffering == 0; }

        /// What share of the picture moved, as a percentage.
        double getPercent() const;
    };

    /// Subtracts one picture from another. Mismatched where either is empty or they disagree on
    /// their extents.
    FrameDifference compareFrames(const Rtx::PngImage& before, const Rtx::PngImage& after);

    /// Renders every view and, where `mAgainst` names a previous run, reports what moved.
    ///
    /// Returns a process exit status: non-zero where any view differs, so a run of this composes
    /// with the build command that produced the binary.
    int runVerify(World& world, const Rtx::ValidationOptions& validation, const VerifyRequest& request);
}
