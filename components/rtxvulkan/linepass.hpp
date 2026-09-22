#pragma once

#include <cstdint>
#include <filesystem>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/line.h>

#include "graphicspipeline.hpp"

namespace Rtx
{
    class Device;
    class Image;

    /// What one draw of the debug lines is over. A record and not an argument list, because two
    /// of the fields are an `Image` and two more a count, and either pair takes the other's value
    /// without a word.
    struct Lines
    {
        /// What to draw over, in `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL`. Loaded rather than
        /// cleared: the frame is already in it.
        const Image& mTarget;

        /// The trace's depth channel, in `GENERAL`, at the extent `mConstants.mTraced` names.
        const Image& mDepth;

        Shaders::LineConstants mConstants;

        /// The lines' vertices first and the triangles' after them, in `Rtx::DebugVertex` layout:
        /// `mLineCount` and then `mTriangleCount` of them.
        VkBuffer mVertices = VK_NULL_HANDLE;
        std::uint32_t mLineCount = 0;
        std::uint32_t mTriangleCount = 0;
    };

    /// The game's debug lines and triangles, over the finished picture and under the interface:
    /// `shaders/line.h` says what they are and why they are rasterized. Beside `GuiPass` in shape
    /// — the same dynamic rendering over the same target — and unlike it in every fragment
    /// asking the trace whether it stands in front of what was drawn there.
    class LinePass
    {
    public:
        /// @param targetFormat the format of the image this will draw over, fixed at construction
        ///        because a pipeline is compiled against it.
        LinePass(const Device& device, const std::filesystem::path& shaderDirectory, VkFormat targetFormat);

        void record(VkCommandBuffer commands, const Lines& what) const;

    private:
        /// Two, because a topology is baked into a pipeline: the navmesh is triangles and its
        /// edges, the pathgrid its lines, the collision shapes both.
        GraphicsPipeline mLines;
        GraphicsPipeline mTriangles;
    };
}
