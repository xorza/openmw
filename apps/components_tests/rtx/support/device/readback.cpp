#include "readback.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtxvulkan/barriers.hpp>
#include <components/rtxvulkan/imageuse.hpp>

namespace Rtx::Testing
{
    namespace
    {
        /// One half float, as the number it stands for.
        ///
        /// **Spelled out rather than shared with the renderer, and by arithmetic rather than by
        /// bits.** Several passes keep their output in halves, so a test that read them through the
        /// same helper the shader used would pass however wrong that helper was — and one written in
        /// shifts and masks is a second place for the subnormal case to be wrong.
        float fromHalf(std::uint16_t bits)
        {
            const float sign = (bits & 0x8000u) != 0 ? -1.0f : 1.0f;
            const int exponent = (bits >> 10) & 0x1f;
            const int mantissa = bits & 0x3ff;

            if (exponent == 0)
                return sign * std::ldexp(static_cast<float>(mantissa), -24);

            if (exponent == 31)
                return sign
                    * (mantissa == 0 ? std::numeric_limits<float>::infinity()
                                     : std::numeric_limits<float>::quiet_NaN());

            return sign * std::ldexp(1.0f + static_cast<float>(mantissa) / 1024.0f, exponent - 15);
        }
    }

    void orderStorageWrites(VkCommandBuffer commands)
    {
        handOver(commands, Use::sBufferComputeWrite,
            BufferUse{ Use::sBufferComputeReadWrite.mStage | Use::sBufferHostRead.mStage,
                Use::sBufferComputeReadWrite.mAccess | Use::sBufferHostRead.mAccess });
    }

    Image makeTestImage(
        const Device& device, const VkExtent2D extent, const VkFormat format, const std::string_view name)
    {
        // `SAMPLED` because an upscaler samples its inputs and an image it cannot sample reads as
        // zero — no error, no validation message, a black frame. Both transfer bits so a clear can
        // fill it and the result can be read back.
        return Image(device, extent.width, extent.height, format,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            name);
    }

    std::vector<float> readHalves(const Image& image, std::uint32_t level)
    {
        std::vector<std::uint8_t> bytes;
        image.read(VK_IMAGE_LAYOUT_GENERAL, bytes, level);

        std::vector<float> values(bytes.size() / sizeof(std::uint16_t));
        for (std::size_t at = 0; at < values.size(); ++at)
        {
            std::uint16_t bits = 0;
            std::memcpy(&bits, bytes.data() + at * sizeof(bits), sizeof(bits));
            values[at] = fromHalf(bits);
        }

        return values;
    }
}
