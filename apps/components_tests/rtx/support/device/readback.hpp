#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/image.hpp>

namespace Rtx::Testing
{
    /// Orders one compute pass over a storage buffer against the next, and against a host read of
    /// what the last of them left.
    ///
    /// **Written as well as read, for the reason `WavePass::order` gives.** A transform that runs in
    /// place reads its buffer and writes it back, so what follows a pass is a write after a write as
    /// much as a read after one, and a dependency naming only the read leaves the two writes
    /// unordered.
    void orderStorageWrites(VkCommandBuffer commands);

    /// The four bytes at a pixel of an RGBA8 image `width` texels across, row zero at the top.
    inline std::array<std::uint8_t, 4> rgbaAt(
        std::span<const std::uint8_t> pixels, std::uint32_t width, std::uint32_t x, std::uint32_t y)
    {
        const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4;

        return { pixels[offset], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3] };
    }

    /// Every channel of one level of a half-float image, decoded, row major.
    ///
    /// **Left in the layout it was found in**, which `Image::read` promises: reading an image is not
    /// a change to it. Several passes keep their output in halves, so this is the read-back beside
    /// the decoder rather than one copy of it per suite.
    std::vector<float> readHalves(const Image& image, std::uint32_t level = 0);

    /// An image a pass can be handed as its frame: written as storage, sampled, and copied both
    /// ways, so a test can fill it and read it back. One level.
    Image makeTestImage(const Device& device, VkExtent2D extent, VkFormat format, std::string_view name);
}
