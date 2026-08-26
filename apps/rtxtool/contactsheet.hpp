#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace Rtx
{
    struct TextureData;
}

namespace RtxTool
{
    /// A drawn contact sheet, before anything has decided what to do with it.
    struct ContactSheet
    {
        std::vector<std::uint8_t> mPixels;
        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;

        /// How many textures were drawn, which is how many pairs the grid holds.
        std::uint32_t mCount = 0;

        /// Where a pair's left thumbnail starts, in pixels from the top left of the sheet.
        ///
        /// Exposed so a test can read what was drawn rather than trusting the picture. The de-lit
        /// half of the same pair starts `getStride()` further right.
        std::uint32_t getLeftOf(std::uint32_t index) const;
        std::uint32_t getTopOf(std::uint32_t index) const;

        /// How far apart the two halves of a pair stand.
        static std::uint32_t getStride();

        /// How wide one thumbnail is drawn.
        static std::uint32_t getThumbnail();
    };

    /// Writes every texture a cell uses, vanilla beside de-lit, as one image.
    ///
    /// **De-lighting is the one thing in this renderer a number cannot judge.** An estimate that
    /// removes the light painted into a texture and one that removes the *detail* painted into it
    /// move every metric the same way — the frame changes by about as much either way, and both
    /// look like progress. The only way to tell them apart is to put the two side by side and look.
    ///
    /// The thumbnails are nearest-neighbour on purpose: a smoothed one would hide exactly the loss
    /// of detail this exists to find.
    ///
    /// @param strength the same `--delight` the frame runs at, so the sheet shows what the frame
    ///        is actually doing rather than what it could do.
    ContactSheet drawContactSheet(std::span<const Rtx::TextureData> textures, float strength);

    /// The same, written where it can be looked at. Empty where the cell used no textures.
    ContactSheet writeContactSheet(
        std::span<const Rtx::TextureData> textures, const std::filesystem::path& out, float strength);
}
