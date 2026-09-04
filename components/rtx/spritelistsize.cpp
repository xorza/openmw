#include "spritelistsize.hpp"

#include <algorithm>

namespace Rtx
{
    void SpriteListSize::sizeFor(const std::uint32_t tiles, const std::uint32_t sprites, const std::uint32_t reported)
    {
        mTiles = tiles;

        // **Every step in sixty-four bits.** A share of the tiles is a product of two counts and
        // twice a report is a doubling of a number the device wrote, and either overflows a
        // `std::uint32_t` for a frame that is merely large. An overflow here is the worst shape a
        // wrong answer can take: a smaller number, under a capacity that did not shrink with it.
        const std::uint64_t floor = std::uint64_t{ sprites } * tiles / sFloorShare;
        const std::uint64_t wanted = std::max({ std::uint64_t{ mCapacity }, floor, 2 * std::uint64_t{ reported } });

        mCapacity = static_cast<std::uint32_t>(std::min(wanted, std::uint64_t{ sMostEntries }));
    }
}
