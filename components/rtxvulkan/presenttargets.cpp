#include "presenttargets.hpp"

#include "commands.hpp"
#include "device.hpp"

namespace Rtx
{
    void PresentTargets::resize(
        const Device& device, CommandPool& pool, const std::uint32_t width, const std::uint32_t height)
    {
        const auto make = [&](const char* const name) {
            return std::make_unique<Image>(device, width, height, sFormat,
                // Drawn into as well as written: the tone curve writes it as a storage image and the
                // GUI rasterises over what that left.
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                    | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                name);
        };

        mTarget = make("target 0");
        mSpare = make("target 1");
        mPresented = nullptr;

        pool.submitAndWait([&](VkCommandBuffer commands) {
            const VkClearColorValue black{ .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
            const VkImageSubresourceRange whole{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

            for (const Image* target : { mTarget.get(), mSpare.get() })
            {
                target->transition(commands, Use::sUndefined, Use::sClearWrite);

                vkCmdClearColorImage(
                    commands, target->getHandle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &whole);

                target->transition(commands, Use::sClearWrite, Use::sAnyGeneral);
            }
        });
    }
}
