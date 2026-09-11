#pragma once

#include <array>
#include <cstdint>

#include <components/rtx/renderer.hpp>

namespace Rtx::Testing
{
    /// A packed vertex colour, in the order MyGUI writes one: red in the low byte.
    constexpr std::uint32_t packColour(std::uint8_t red, std::uint8_t green, std::uint8_t blue, std::uint8_t alpha)
    {
        return static_cast<std::uint32_t>(red) | (static_cast<std::uint32_t>(green) << 8)
            | (static_cast<std::uint32_t>(blue) << 16) | (static_cast<std::uint32_t>(alpha) << 24);
    }

    /// Two triangles of a rectangle in clip space, with texture coordinates over the whole of it.
    ///
    /// Given MyGUI's orientation rather than Vulkan's: `top` is the coordinate nearer +1, because
    /// MyGUI computes its vertices for a clip space with +Y up.
    ///
    /// **Shared by the pass's own test and the renderer's**, which asked the same question of two
    /// levels and wrote this out twice to do it. Named for the interface rather than the shape,
    /// because `extractor/fixture.hpp` already has a `makeQuad` and it builds an `osg::Geometry`.
    inline std::array<GuiVertex, 6> makeGuiQuad(float left, float top, float right, float bottom, std::uint32_t colour)
    {
        const GuiVertex topLeft{ left, top, 0.0f, colour, 0.0f, 0.0f };
        const GuiVertex topRight{ right, top, 0.0f, colour, 1.0f, 0.0f };
        const GuiVertex bottomLeft{ left, bottom, 0.0f, colour, 0.0f, 1.0f };
        const GuiVertex bottomRight{ right, bottom, 0.0f, colour, 1.0f, 1.0f };

        return { topLeft, bottomLeft, bottomRight, topLeft, bottomRight, topRight };
    }
}
