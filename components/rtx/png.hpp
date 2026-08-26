#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace Rtx
{
    /// Writes tightly packed 8-bit RGBA, top row first, as a PNG.
    ///
    /// The renderer writes row zero at the top and OSG's images start at the bottom, so this flips
    /// on the way through. Throws when the file cannot be written.
    void writePng(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
        std::span<const std::uint8_t> pixels);

    /// A picture in the layout `writePng` takes: tightly packed 8-bit RGBA, top row first.
    struct PngImage
    {
        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;
        std::vector<std::uint8_t> mPixels;

        bool empty() const { return mPixels.empty(); }
    };

    /// Reads a PNG back into that layout. Empty where the file is missing or is not eight-bit
    /// colour, which a caller comparing two runs reports rather than throws over.
    PngImage readPng(const std::filesystem::path& path);
}
