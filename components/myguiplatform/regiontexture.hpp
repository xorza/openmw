#ifndef OPENMW_COMPONENTS_MYGUIPLATFORM_REGIONTEXTURE_H
#define OPENMW_COMPONENTS_MYGUIPLATFORM_REGIONTEXTURE_H

#include <cstdint>
#include <span>

namespace MyGUIPlatform
{

    /// A GUI texture that can be written in part.
    ///
    /// **Not something MyGUI can ask for.** `ITexture` hands out a buffer for the whole surface and
    /// takes it back filled; a picture that changes in one corner has nowhere to say so. The world
    /// map is what makes that hurt — entering a cell repaints eighteen pixels square and sends two
    /// megabytes — so where a backend can do better than the interface allows, this is where it says
    /// so.
    ///
    /// **A texture may reasonably not implement this**, and a caller that finds it cannot writes the
    /// whole surface instead. `Picture::setRegion` is that caller.
    class RegionTexture
    {
    public:
        virtual ~RegionTexture() = default;

        /// Copies `rows` into the rectangle at `x`, `y`.
        ///
        /// `rows` is `height` rows of `width` pixels, four bytes each, tightly packed — the region's
        /// own rows and not slices of a wider image. The rectangle must lie inside the texture,
        /// which is a contract on the caller.
        virtual void writeRegion(std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height,
            std::span<const std::uint8_t> rows)
            = 0;
    };

}

#endif
