#include "presenttargets.hpp"

#include "commands.hpp"
#include "device.hpp"
#include "imageuse.hpp"

namespace Rtx
{
    void PresentTargets::resize(const Device& device, const std::uint32_t width, const std::uint32_t height)
    {
        const auto make = [&](const char* const name) {
            return Image(device, width, height, sFormat,
                // Drawn into as well as written: the tone curve writes it as a storage image and the
                // GUI rasterises over what that left.
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                    | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                name);
        };

        mTarget = make("target 0");
        mSpare = make("target 1");
        mSparePresented = false;
        mClaimed = false;

        device.getPool().submitAndWait([&](VkCommandBuffer commands) {
            const VkClearColorValue black{ .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };

            for (const Image* target : { &mTarget, &mSpare })
                target->clear(commands, Use::sUndefined, black, Use::sAnyGeneral);
        });
    }
}
