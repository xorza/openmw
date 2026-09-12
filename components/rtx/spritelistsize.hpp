#pragma once

#include <cstdint>

namespace Rtx
{
    /// How long one copy of the sprite tile list is, and how much of it a bin may fill — both
    /// taken off one high-water mark, because two statements of `tiles + 1` starts plus the runs
    /// once wrapped in thirty-two bits with the pass still holding the old capacity, a dispatch
    /// past the end of an allocation. It grows and never shrinks. A frame the sizing misjudges is
    /// slow and not wrong: the list falls back to `SPRITE_LIST_UNBINNED`, and the next frame is
    /// sized to what the device reported.
    class SpriteListSize
    {
    public:
        /// The most room the runs are ever given, so the sum cannot wrap the shader's thirty-two
        /// bits and a report the device wrote cannot ask for an allocation no card has. Sixteen
        /// million entries is sixty-four megabytes: room for four hundred thousand drops of the
        /// rain over Balmora.
        static constexpr std::uint32_t sMostEntries = 16u << 20;

        /// What share of the frame's tiles a sprite is given room for before any bin has said what
        /// it needs: one tile in this many. A floor for the frame nothing came before, under a
        /// policy that otherwise gives twice what the last bin reported. A share and not a count,
        /// because a sprite's rectangle grows with the tile count: rain over Balmora measured
        /// thirty-five entries a drop over three thousand six hundred tiles, and a count of
        /// thirty-two was outgrown on the first frame.
        static constexpr std::uint32_t sFloorShare = 64;

        /// Takes the frame about to be binned into account, and moves the mark where it has to.
        ///
        /// @param tiles what `Shaders::spriteTilesIn` says that frame's camera covers.
        /// @param reported what the last bin into this copy said its runs came to, whether or not
        ///        they fit. Nought where none has run.
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
