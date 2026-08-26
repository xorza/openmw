#pragma once

#include <cstdint>
#include <filesystem>
#include <span>

#include <vulkan/vulkan_core.h>

#include "graphicspipeline.hpp"

namespace Rtx
{
    class Device;
    class Image;

    /// One run of vertices drawn with one texture.
    ///
    /// **A run and not an index range**, because MyGUI hands over triangle lists and no indices: a
    /// batch is a stretch of the vertex buffer and a texture to read while drawing it.
    struct GuiDraw
    {
        VkImageView mTexture = VK_NULL_HANDLE;
        std::uint32_t mFirstVertex = 0;
        std::uint32_t mVertexCount = 0;

        /// Which of the pass's two pipelines draws it. Runs are recorded in the order given and the
        /// pipeline is bound again only where it changes, so a caller that keeps like with like
        /// pays for one bind.
        Blend mBlend = Blend::Over;
    };

    /// The GUI, over the finished picture.
    ///
    /// **After tone mapping and before present**, which makes it the only pass here working in
    /// display-referred colour. MyGUI picked its colours and drew its atlases looking at a monitor;
    /// putting them through a curve meant for radiance is how a menu comes out grey.
    ///
    /// **The only triangles in this backend.** Everything that makes the picture is dispatched, and
    /// this is not an exception to that so much as an admission that a font atlas is not something
    /// to trace.
    class GuiPass
    {
    public:
        /// @param targetFormat the format of the image this will draw over. Fixed at construction
        ///        because a pipeline is compiled against it; a resize does not change it.
        GuiPass(const Device& device, const std::filesystem::path& shaderDirectory, VkFormat targetFormat);
        ~GuiPass();

        GuiPass(const GuiPass&) = delete;
        GuiPass& operator=(const GuiPass&) = delete;

        /// @param target what to draw over, in `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL` and made
        ///        with `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT`, which is asserted. Loaded rather than
        ///        cleared: the frame is already in it.
        /// @param vertices every batch's vertices in one buffer, in `Rtx::GuiVertex` layout.
        /// @param draws what to draw and what to read while drawing it, in order. Each texture must
        ///        be in `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`.
        void record(
            VkCommandBuffer commands, const Image& target, VkBuffer vertices, std::span<const GuiDraw> draws) const;

    private:
        const Device& mDevice;

        /// **Two, because a blend mode is baked into a pipeline.** The alternative is
        /// `VK_EXT_extended_dynamic_state3`, which is a device feature to require and a driver
        /// path to trust for something that is two objects compiled once at startup.
        GraphicsPipeline mOver;
        GraphicsPipeline mAdditive;

        VkSampler mSampler = VK_NULL_HANDLE;
    };
}
