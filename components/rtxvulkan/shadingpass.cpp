#include "shadingpass.hpp"

#include <array>

#include <components/rtx/shaders/shadingmap.h>
#include <components/rtx/texturedata.hpp>

#include "buffer.hpp"
#include "dispatch.hpp"
#include "image.hpp"
#include "imageuse.hpp"

namespace Rtx
{
    namespace
    {
        /// The texture in, the sums out.
        constexpr std::array<VkDescriptorSetLayoutBinding, 2> sSumBindings{
            computeBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            computeBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
        };

        /// The sums in, the map out.
        constexpr std::array<VkDescriptorSetLayoutBinding, 2> sMapBindings{
            computeBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            computeBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        };

        constexpr VkDeviceSize sSumBytes
            = VkDeviceSize{ Shaders::SHADING_EXTENT } * Shaders::SHADING_EXTENT * sizeof(Shaders::ShadingSum);
    }

    ShadingPass::ShadingPass(const Device& device, const std::filesystem::path& shaderDirectory)
        : mSum(device, sSumBindings, sizeof(Shaders::ShadingConstants), {}, shaderDirectory / "shadingsum.comp.spv",
            "shading sum")
        , mMap(device, sMapBindings, sizeof(Shaders::ShadingConstants), {}, shaderDirectory / "shadingmap.comp.spv",
              "shading map")
        , mSums(Buffer::deviceLocal(device, sSumBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "shading sums"))
    {
    }

    void ShadingPass::record(const VkCommandBuffer commands, const Image& source, const VkSampler sampler,
        const Image& map, const TextureData& data) const
    {
        // Against the map stage of the texture before this one, which read the sums this is
        // about to write over. An execution dependency is all a write-after-read needs.
        mSums.transition(commands, Use::sBufferComputeRead, Use::sBufferComputeWrite);

        const Shaders::ShadingConstants constants{
            .mWidth = data.mWidth,
            .mHeight = data.mHeight,
            .mPunchThrough = data.mFormat == TextureFormat::Bc1RgbaSrgb ? 1u : 0u,
        };

        DescriptorWrites<2> summing;
        summing.image(0, source.describeSampled(sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        summing.buffer(1, mSums.describe());
        dispatch(commands, mSum, summing.get(), constants, Shaders::SHADING_EXTENT);

        mSums.transition(commands, Use::sBufferComputeWrite, Use::sBufferComputeRead);
        map.transition(commands, Use::sUndefined, Use::sComputeWrite);

        DescriptorWrites<2> mapping;
        mapping.buffer(0, mSums.describe());
        mapping.image(1, map.describeStorage());
        dispatch(commands, mMap, mapping.get(), constants, 1);

        map.transition(commands, Use::sComputeWrite, Use::sTextureSample);
    }
}
