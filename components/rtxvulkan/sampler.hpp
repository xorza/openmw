#pragma once

#include <string_view>
#include <utility>

#include <vulkan/vulkan_core.h>

#include "owned.hpp"

namespace Rtx
{
    class Device;

    /// A sampler, its lifetime, and the two shapes this renderer reads images through.
    ///
    /// **Two configurations, and every sampler in this renderer is one of them.** All of them are
    /// linear. What differs is whether an image tiles and whether it carries a chain, and those go
    /// together: a tiling image is content and content is mipped, while a pass's own target is
    /// neither.
    ///
    /// **Written out per class, the pair drifts where nothing decides.** A `maxLod` left at nought
    /// reached nothing only because the images below it happened to have one level; a border colour
    /// was set on a sampler that clamps to the edge and so never reads one. Neither was a decision,
    /// and neither could have been argued with while the create-info sat in seven places.
    class Sampler
    {
    public:
        /// Clamped to the edge, over the whole chain. What a pass reads its own targets through: a
        /// bloom level, a volume slice, a frame. Clamped because such a target runs to the edge of
        /// what it was given, and wrapping would fetch the far side of it.
        static Sampler forTarget(const Device& device, std::string_view name);

        /// Repeating, over the whole chain. What content is read through, because Morrowind's
        /// textures tile and a great many of them rely on it.
        static Sampler forContent(const Device& device, std::string_view name);

        VkSampler get() const { return mHandle.get(); }

    private:
        /// **The only way to make one, so there is no empty `Sampler`.** A member of this type holds
        /// a sampler from the moment its owner is constructed, which is what lets every holder bind
        /// it without asking whether it is there.
        explicit Sampler(Owned<VkSampler, vkDestroySampler> handle)
            : mHandle(std::move(handle))
        {
        }

        Owned<VkSampler, vkDestroySampler> mHandle;
    };
}
