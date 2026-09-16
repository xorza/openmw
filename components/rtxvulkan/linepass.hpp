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

        /// @param target what to draw over, in `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL`. Loaded
        ///        rather than cleared: the frame is already in it.
        /// @param depth the trace's depth channel, in `GENERAL`, at the extent `constants.mTraced`
        ///        names.
        /// @param vertices the lines' vertices first and the triangles' after them, in
        ///        `Rtx::DebugVertex` layout: `lineCount` and then `triangleCount` of them.
        void record(VkCommandBuffer commands, const Image& target, const Image& depth,
            const Shaders::LineConstants& constants, VkBuffer vertices, std::uint32_t lineCount,
            std::uint32_t triangleCount) const;

    private:
        /// Two, because a topology is baked into a pipeline: the navmesh is triangles and its
        /// edges, the pathgrid its lines, the collision shapes both.
        GraphicsPipeline mLines;
        GraphicsPipeline mTriangles;
    };
}
