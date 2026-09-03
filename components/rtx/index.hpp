#pragma once

#include <cstdint>

namespace Rtx
{
    /// An index into one of `SceneDesc`'s tables, or `sNoIndex` for "none".
    ///
    /// **Its own header because the tables are their own types.** `TextureTable` and
    /// `PlacementTable` are members of `SceneDesc` and cannot include the header that includes them.
    using Index = std::uint32_t;

    inline constexpr Index sNoIndex = ~Index{ 0 };
}
