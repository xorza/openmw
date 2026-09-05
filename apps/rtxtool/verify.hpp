#pragma once

#include <cstdint>
#include <filesystem>
#include <span>

#include <components/rtx/png.hpp>

#include "views.hpp"

namespace RtxTool
{

    /// What two renderings of one view came to.
    ///
    /// **A magnitude and not a verdict.** "Worst 2 of 255 on 5% of the pixels" is a rounding
    /// difference and "worst 37 on 20%" is a bug, and a bare *differs* — which is all this fork had
    /// — cost a day of bisection that reached the wrong answer twice.
    ///
    /// **"Worst 25 on four hundredths of a per cent", the same pixels every time, in one run of
    /// several, was the driver and not this tree — and it is pinned now.** Addamasartus flipped
    /// between two pictures nine hundred pixels apart with everything handed to the device hashing
    /// the same, and Arkngthand moved 13% of its albedo by one of 255. That was the driver's second
    /// compile of the trace pipeline, made on its own thread seconds after the first, fusing the
    /// primary ray's sum into different multiply-adds; `rayAt` says the rest and is `precise` for it.
    /// A run now draws one picture, and a difference this reports is a change.
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

    /// Reads back what a run wrote and says what moved since `against`.
    ///
    /// **What this is for is saying whether a change moved the picture, and by how much.** A
    /// refactor of the geometry path is supposed to leave every frame exactly as it was, and the
    /// only thing that can say so is the previous build's own frames — so the reference is a
    /// directory an earlier run wrote on this machine, never a corpus in the tree. The picture is a
    /// function of the driver and the card as much as of the code, and checked-in bytes would be a
    /// promise the tree cannot keep.
    ///
    /// Returns a process exit status: non-zero where any view differs, so a run of this composes
    /// with the build command that produced the binary. Zero where `against` is empty, which is a
    /// run that only wrote a reference for the next one.
    int compareRuns(
        const std::filesystem::path& wrote, const std::filesystem::path& against, std::span<const View> views);
}
