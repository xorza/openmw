#include "texture.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <components/rtx/error.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/shadingmap.hpp>

#include "buffer.hpp"
#include "commands.hpp"
#include "device.hpp"
#include "graveyard.hpp"
#include "result.hpp"

namespace Rtx
{
    namespace
    {
        /// How many textures a scene may hold.
        ///
        /// The descriptor array is sized once and bound for the run; a cell of Morrowind reaches a
        /// couple of hundred, and a worldspace will not reach this.
        constexpr std::uint32_t sMaxTextures = 4096;

        /// The one place a `TextureFormat` becomes Vulkan's.
        ///
        /// Every case is sRGB, and `TextureFormat` says why: the files hold display-encoded bytes
        /// and the hardware converts them in the filter, which is what hands the shader linear
        /// values for free.
        VkFormat toVulkanFormat(TextureFormat format)
        {
            switch (format)
            {
                case TextureFormat::Bc1RgbaSrgb:
                    return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
                case TextureFormat::Bc2Srgb:
                    return VK_FORMAT_BC2_SRGB_BLOCK;
                case TextureFormat::Bc3Srgb:
                    return VK_FORMAT_BC3_SRGB_BLOCK;
                case TextureFormat::Rgba8Unorm:
                    return VK_FORMAT_R8G8B8A8_UNORM;
                case TextureFormat::Rgba8Srgb:
                    return VK_FORMAT_R8G8B8A8_SRGB;
                case TextureFormat::Bgra8Srgb:
                    return VK_FORMAT_B8G8R8A8_SRGB;
            }

            // Unreachable for any value of the enumeration; a new one that forgets a case lands
            // here rather than creating an image with a format nobody chose.
            throw Error("unknown texture format");
        }

        /// The two arrays the set holds: the textures, and their shading maps at the same slots.
        constexpr std::uint32_t sTextureBinding = 0;
        constexpr std::uint32_t sShadingBinding = 1;

        /// Every stage that resolves a hit, and every dispatch. The trace reads these arrays from
        /// its closest-hit shaders, from the any-hit shader that tests a cutout and from the miss
        /// shader that draws the sky; the fog volume, the tone curve and the interface are
        /// dispatches and read them too.
        constexpr VkShaderStageFlags sStages = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR
            | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR;

        /// The layout every array declares, which is the same layout whatever the scene holds.
        ///
        /// **Sized to the maximum and not to the scene, because a pipeline outlives a cell.** Two
        /// set layouts are compatible only where they are identically defined, so a layout that
        /// counted the scene's textures made every cell's array incompatible with the pipeline
        /// layout built from the last one's — and a renderer that keeps its pass across scenes, as
        /// this one does because building one compiles a shader, would bind a set the pipeline
        /// cannot accept. The count moves to the allocation, where it costs what the scene actually
        /// uses.
        SetLayout makeLayout(const Device& device)
        {
            const std::array<VkDescriptorSetLayoutBinding, 2> bindings{
                VkDescriptorSetLayoutBinding{
                    sTextureBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sMaxTextures, sStages },
                VkDescriptorSetLayoutBinding{
                    sShadingBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sMaxTextures, sStages },
            };

            // Partially bound because a scene with fewer textures than the array can hold leaves the
            // tail unwritten, and a shader that never indexes there must not be told it is an error.
            // Variable count is what keeps the declared maximum from being what gets allocated, and
            // it is allowed on the last binding of a set alone, which is the one it is on.
            constexpr std::array<VkDescriptorBindingFlags, 2> flags{
                VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
                VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT,
            };
            const VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlags{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
                .bindingCount = static_cast<std::uint32_t>(flags.size()),
                .pBindingFlags = flags.data(),
            };

            return SetLayout(device, bindings, 0, &bindingFlags);
        }

        /// A set of `layout` from a pool of its own: the texture binding at the maximum the layout
        /// declares, and the shading binding — the variable one — at `shadingCount`.
        ///
        /// What the layout left open: the arrays are declared at their maximum, and the count an
        /// allocation names is what says how many of the last binding's descriptors this set pays
        /// for.
        SetApart allocateSet(const Device& device, VkDescriptorSetLayout layout, std::uint32_t shadingCount)
        {
            SetApart set;

            const VkDescriptorPoolSize size{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sMaxTextures + shadingCount };
            const VkDescriptorPoolCreateInfo describePool{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                .maxSets = 1,
                .poolSizeCount = 1,
                .pPoolSizes = &size,
            };
            checkVk(vkCreateDescriptorPool(device.getHandle(), &describePool, nullptr, &set.mPool),
                "vkCreateDescriptorPool");

            const VkDescriptorSetVariableDescriptorCountAllocateInfo variable{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
                .descriptorSetCount = 1,
                .pDescriptorCounts = &shadingCount,
            };
            const VkDescriptorSetAllocateInfo allocate{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .pNext = &variable,
                .descriptorPool = set.mPool,
                .descriptorSetCount = 1,
                .pSetLayouts = &layout,
            };
            checkVk(vkAllocateDescriptorSets(device.getHandle(), &allocate, &set.mSet), "vkAllocateDescriptorSets");

            return set;
        }

        /// Copies `bytes` into `image` by `regions`, and leaves it where a sampler expects it.
        ///
        /// **Recorded rather than submitted.** A cell brings hundreds of these and the queue is asked
        /// once for all of them; the image is left where a sampler expects it, so nothing recorded
        /// afterwards has to know this one happened.
        void upload(const Device& device, Batch& batch, Image& image, std::span<const std::byte> bytes,
            std::span<const VkBufferImageCopy> regions)
        {
            Buffer staging = Buffer::staging(device, bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            staging.write(bytes);

            const VkCommandBuffer commands = batch.getCommands();

            image.transition(commands, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

            vkCmdCopyBufferToImage(commands, staging.getHandle(), image.getHandle(),
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<std::uint32_t>(regions.size()), regions.data());

            image.transition(commands, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

            batch.keep(std::move(staging));
        }
    }

    Texture::Texture(const Device& device, Batch& batch, const TextureData& data, std::string_view name,
        std::vector<VkBufferImageCopy>& regions)
    {
        assert(!data.mLevels.empty());

        const auto levels = static_cast<std::uint32_t>(data.mLevels.size());
        mImage = std::make_unique<Image>(device, data.mWidth, data.mHeight, toVulkanFormat(data.mFormat),
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, name, levels);

        // Every level in one submit: the levels are already contiguous in the source, so this is one
        // copy per level out of one buffer rather than one upload per level.
        regions.clear();
        regions.reserve(levels);
        for (std::uint32_t level = 0; level < levels; ++level)
            regions.push_back(VkBufferImageCopy{
                .bufferOffset = data.mLevels[level].mOffset,
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 },
                .imageExtent = { data.mLevels[level].mWidth, data.mLevels[level].mHeight, 1 },
            });

        upload(device, batch, *mImage, data.mBytes, regions);

        // **The map, in the same batch and left where the same sampler expects it.** One level and
        // no chain: the map is read at level nought whatever the cone, because it has no detail for
        // a level to lose.
        const std::array<std::uint16_t, ShadingMap::sCells> stored = encodeShadingMap(data.mShading);

        // Built only where something reads it. A release build names no object, and the
        // concatenation is past what a short string holds — so building it anyway is one trip to the
        // heap per texture, for a name that goes nowhere.
        std::string shadingName;
        if constexpr (Device::wantsNames())
            shadingName = std::string(name) + " shading";

        mShading = std::make_unique<Image>(device, Shaders::SHADING_EXTENT, Shaders::SHADING_EXTENT,
            VK_FORMAT_R16_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, shadingName);

        const VkBufferImageCopy region{
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { Shaders::SHADING_EXTENT, Shaders::SHADING_EXTENT, 1 },
        };
        upload(device, batch, *mShading, std::as_bytes(std::span(stored)), std::span(&region, 1));

        mBytes = data.mBytes.size() + sizeof(stored);
    }

    TextureArray::TextureArray(const Device& device, Batch& batch, std::uint32_t slots,
        std::span<const TextureData> textures, Graveyard& graveyard)
        : mDevice(device)
        , mLayout(makeLayout(device))
    {
        if (slots > sMaxTextures)
            throw Error("a scene with " + std::to_string(slots) + " textures is past the "
                + std::to_string(sMaxTextures) + " this array holds");

        const VkSamplerCreateInfo sampler{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            // Morrowind's textures tile, and a great many of them rely on it.
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            // Off, and not an oversight: every fetch names its own level, and anisotropic filtering
            // only applies to the implicit and gradient forms. A cone is isotropic by construction.
            .anisotropyEnable = VK_FALSE,
            .maxLod = VK_LOD_CLAMP_NONE,
        };
        checkVk(vkCreateSampler(device.getHandle(), &sampler, nullptr, &mSampler), "vkCreateSampler");

        // **Allocated at the maximum the layout declares, not at what this scene brought.** Sizing
        // the set to the cell is what made a texture arriving mean a new set, a new pool and every
        // image uploaded again; four thousand descriptors is a few hundred kilobytes of pool and it
        // is paid once. `extend` then only ever writes the range that is new.
        const SetApart own = allocateSet(device, mLayout.getHandle(), sMaxTextures);
        mPool = own.mPool;
        mSet = own.mSet;

        // **Sized to the table before anything is written into it**, so a description lands in the
        // slot it names whatever sits either side of it. Every entry starts holding no image and no
        // map, which is what a free slot goes on holding: its descriptors are never written, and
        // the bindings' `PARTIALLY_BOUND` is what makes that legal for one nothing samples.
        mTextures.resize(slots);

        write(batch, textures, graveyard);
    }

    void TextureArray::reserveSlot(std::uint32_t slot)
    {
        if (slot >= sMaxTextures)
            throw Error("a scene wanting texture slot " + std::to_string(slot) + " is past the "
                + std::to_string(sMaxTextures) + " this array holds");

        // Grown to reach it rather than one at a time: arrivals come in whatever order the scene's
        // free list handed the slots out, so the highest is not always the last.
        if (slot >= mTextures.size())
            mTextures.resize(slot + 1);
    }

    void TextureArray::write(Batch& batch, std::span<const TextureData> arrived, Graveyard& graveyard)
    {
        if (arrived.empty())
            return;

        for (const TextureData& texture : arrived)
        {
            reserveSlot(texture.mSlot);

            // Named only where a capture or a validation message could read it back. A local,
            // because a slot number is short enough that this never reaches the heap; the one that
            // does is the map's, inside `Texture`.
            std::string name;
            if constexpr (Device::wantsNames())
                name = "texture " + std::to_string(texture.mSlot);

            // What the slot held is buried and not destroyed: its descriptor is the one a frame in
            // flight bound, and it stays valid until that frame's fence says nothing reads it.
            graveyard.bury(
                std::exchange(mTextures[texture.mSlot], Texture(mDevice, batch, texture, name, mRegionScratch)));
        }

        describe(arrived);
    }

    void TextureArray::drop(std::span<const std::uint32_t> slots, Graveyard& graveyard)
    {
        for (const std::uint32_t slot : slots)
        {
            // A slot this array never held: a scene can add a texture and sweep it in the same
            // window, before anything was handed over to upload it.
            if (slot >= mTextures.size())
                continue;

            // Exchanged rather than erased, so the slot stays where it is and the image goes under
            // the frame that may still name it.
            graveyard.bury(std::exchange(mTextures[slot], Texture()));
        }
    }

    void TextureArray::describe(std::span<const TextureData> arrived)
    {
        if (arrived.empty())
            return;

        // One write per slot and per array rather than one over a range: the arrivals are wherever
        // the scene's free list put them, and a run is no longer what they are. Reserved before
        // any write points into it, since a write names its image by address.
        mImageScratch.clear();
        mWriteScratch.clear();
        mImageScratch.reserve(2 * arrived.size());
        mWriteScratch.reserve(2 * arrived.size());

        for (const TextureData& texture : arrived)
        {
            const Texture& held = mTextures[texture.mSlot];
            queueWrite(mSet, sTextureBinding, texture.mSlot, held.getView(), mImageScratch, mWriteScratch);
            queueWrite(mSet, sShadingBinding, texture.mSlot, held.getShadingView(), mImageScratch, mWriteScratch);
        }

        vkUpdateDescriptorSets(
            mDevice.getHandle(), static_cast<std::uint32_t>(mWriteScratch.size()), mWriteScratch.data(), 0, nullptr);
    }

    SetApart TextureArray::describeApart(std::span<const std::uint32_t> slots) const
    {
        // The shading binding is the variable one, and nothing reads a map through this set.
        const SetApart apart = allocateSet(mDevice, mLayout.getHandle(), 1);

        // Reserved before any write points into it, for the reason `describe` gives. The array's own
        // scratch, because this is the array's answer and nothing else is describing while it runs.
        mImageScratch.clear();
        mWriteScratch.clear();
        mImageScratch.reserve(slots.size());
        mWriteScratch.reserve(slots.size());

        for (const std::uint32_t slot : slots)
        {
            assert(slot < mTextures.size() && mTextures[slot].getView() != VK_NULL_HANDLE
                && "a set described over a slot holding no texture");
            queueWrite(apart.mSet, sTextureBinding, slot, mTextures[slot].getView(), mImageScratch, mWriteScratch);
        }

        vkUpdateDescriptorSets(
            mDevice.getHandle(), static_cast<std::uint32_t>(mWriteScratch.size()), mWriteScratch.data(), 0, nullptr);

        return apart;
    }

    void TextureArray::queueWrite(const VkDescriptorSet set, const std::uint32_t binding, const std::uint32_t slot,
        const VkImageView view, std::vector<VkDescriptorImageInfo>& images,
        std::vector<VkWriteDescriptorSet>& writes) const
    {
        images.push_back(VkDescriptorImageInfo{
            .sampler = mSampler,
            .imageView = view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        });
        writes.push_back(VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = binding,
            .dstArrayElement = slot,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &images.back(),
        });
    }

    TextureArray::~TextureArray()
    {
        if (mPool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(mDevice.getHandle(), mPool, nullptr);
        if (mSampler != VK_NULL_HANDLE)
            vkDestroySampler(mDevice.getHandle(), mSampler, nullptr);
    }

    TexturesHeld TextureArray::getHeld() const
    {
        TexturesHeld held;

        for (const Texture& texture : mTextures)
        {
            // The view and not the size: a slot stands a texture or it does not, and a content file
            // carrying an empty level is a texture that exists.
            if (texture.getView() == VK_NULL_HANDLE)
                continue;

            ++held.mCount;
            held.mBytes += texture.getBytes();
        }

        return held;
    }
}
