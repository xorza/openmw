#pragma once

#include <cstdint>

namespace Rtx
{
    /// How long one copy of the sprite tile list is, and how much of it a bin may fill.
    ///
    /// **The high-water mark and both numbers taken off it, in one object.** The list is `tiles + 1`
    /// starts and then the runs: the buffer is sized to both together, and the pass is told only the
    /// second. Those two were three statements apart in `SceneBuffers::binSprites`, and the sum was
    /// spelled in thirty-two bits — so a large enough frame wrapped the length the buffer was made
    /// at while the pass kept the capacity it was about to write, which is a dispatch past the end
    /// of an allocation. One call moves the mark here and both numbers come off it afterwards.
    ///
    /// **It grows and never shrinks**, so a copy settles at the busiest frame it has drawn. What a
    /// frame the sizing misjudges costs is a slow frame and not a wrong one: the list carries its
    /// own degenerate form — `SPRITE_LIST_UNBINNED` — the trace walks every sprite for it, and the
    /// next frame into this copy is sized to what the device reported.
    class SpriteListSize
    {
    public:
        /// The most room the runs are ever given.
        ///
        /// **A cap the picture does not depend on**, for the reason above: past it a frame is drawn
        /// slow and right. What it buys is arithmetic that cannot wrap. The sum below stays far
        /// inside the thirty-two bits the shader indexes the list with, and a report the device
        /// wrote — the one number here that this side did not compute — cannot ask for an
        /// allocation no card has.
        ///
        /// Sixteen million entries is sixty-four megabytes. Rain over Balmora measured thirty-five
        /// entries a drop over three thousand six hundred tiles, so this is room for four hundred
        /// thousand drops of it.
        static constexpr std::uint32_t sMostEntries = 16u << 20;

        /// What share of the frame's tiles a sprite is given room for before any bin has said what
        /// it needs: one tile in this many.
        ///
        /// **A floor, under a policy that otherwise follows what the last bin reported.** A frame's
        /// entries are what the last one's were, near enough — a storm gains its drops over seconds
        /// and a puff rises over the same — so twice the last report covers the drift, and the floor
        /// covers the frame nothing came before, which is a cell arriving with a storm already in
        /// it. **As a share of the tiles and not a count**, because a sprite's rectangle grows with
        /// the tile count: rain over Balmora measured thirty-five entries a drop over three thousand
        /// six hundred tiles — a streak near the eye is a column of them — and a count of thirty-two
        /// a sprite was outgrown on the first frame. One in sixty-four is fifty-six there and a
        /// hundred and twenty-seven over the game's own frame.
        static constexpr std::uint32_t sFloorShare = 64;

        /// Takes the frame about to be binned into account, and moves the mark where it has to.
        ///
        /// @param tiles what `Shaders::spriteTilesIn` says that frame's camera covers.
        /// @param sprites how many there are to bin.
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
