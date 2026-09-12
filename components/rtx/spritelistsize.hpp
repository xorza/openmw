#pragma once

#include <cstdint>

namespace Rtx
{
    /// How long one copy of the sprite tile list is, and how much of it a bin may fill, both off
    /// one high-water mark that grows and never shrinks. A frame the sizing misjudges is slow and
    /// not wrong: the list falls back to `SPRITE_LIST_UNBINNED`, and the next frame is sized to
    /// what the device reported.
    class SpriteListSize
    {
    public:
        /// The most room the runs are ever given, so a report the device wrote cannot ask for an
        /// allocation no card has: sixty-four megabytes, room for four hundred thousand drops of
        /// rain over Balmora.
        static constexpr std::uint32_t sMostEntries = 16u << 20;

        /// What share of the frame's tiles a sprite is given room for before any bin has said what
        /// it needs: one tile in this many, under a policy that otherwise gives twice what the last
        /// bin reported. A share and not a count, because a sprite's rectangle grows with the tile
        /// count.
        static constexpr std::uint32_t sFloorShare = 64;

        /// Moves the mark for the frame about to be binned: `tiles` is what `Shaders::spriteTilesIn`
        /// says the camera covers, `reported` what the last bin into this copy came to whether or
        /// not it fit, or nought where none has run.
        void sizeFor(std::uint32_t tiles, std::uint32_t sprites, std::uint32_t reported);

        /// What the pass is told it has room for after the starts.
        std::uint32_t getCapacity() const { return mCapacity; }

        /// Entries the buffer must hold: the starts and the capacity together.
        std::uint64_t getEntries() const { return std::uint64_t{ mTiles } + 1 + mCapacity; }

        /// What that comes to in bytes, which is what the buffer is grown to.
        std::uint64_t getBytes() const { return getEntries() * sizeof(std::uint32_t); }

    private:
        std::uint32_t mTiles = 0;
        std::uint32_t mCapacity = 0;
    };
}
