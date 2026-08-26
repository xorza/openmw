#include "texture.hpp"

#include <algorithm>
#include <cassert>
#include <utility>
#include <vector>

#include <components/rtx/error.hpp>
#include <components/rtx/shaders/scene.h>

#include "buffer.hpp"
#include "commands.hpp"
#include "device.hpp"
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

        /// The layout every array declares, which is the same layout whatever the scene holds.
        ///
        /// **Sized to the maximum and not to the scene, because a pipeline outlives a cell.** Two
        /// set layouts are compatible only where they are identically defined, so a layout that
        /// counted the scene's textures made every cell's array incompatible with the pipeline
        /// layout built from the last one's — and a renderer that keeps its pass across scenes, as
        /// this one does because building one compiles a shader, would bind a set the pipeline
        /// cannot accept. The count moves to the allocation, where it costs what the scene actually
        /// uses.
        VkDescriptorSetLayout makeLayout(const Device& device)
        {
            const VkDescriptorSetLayoutBinding binding{
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = sMaxTextures,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            };

            // Partially bound because a scene with fewer textures than the array can hold leaves the
            // tail unwritten, and a shader that never indexes there must not be told it is an error.
            // Variable count is what keeps the declared maximum from being what gets allocated.
            constexpr VkDescriptorBindingFlags flags
                = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
            const VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlags{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
                .bindingCount = 1,
                .pBindingFlags = &flags,
            };

            const VkDescriptorSetLayoutCreateInfo create{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .pNext = &bindingFlags,
                .bindingCount = 1,
                .pBindings = &binding,
            };

            VkDescriptorSetLayout layout = VK_NULL_HANDLE;
            checkVk(vkCreateDescriptorSetLayout(device.getHandle(), &create, nullptr, &layout),
                "vkCreateDescriptorSetLayout");
            return layout;
        }
    }

    Texture::Texture(const Device& device, Batch& batch, const TextureData& data, std::string_view name)
        : mDevice(device.getHandle())
        , mBytes(data.mBytes.size())
    {
        assert(!data.mLevels.empty());

        const VkFormat format = toVulkanFormat(data.mFormat);
        const auto levels = static_cast<std::uint32_t>(data.mLevels.size());

        const VkImageCreateInfo create{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = { data.mWidth, data.mHeight, 1 },
            .mipLevels = levels,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        checkVk(vkCreateImage(mDevice, &create, nullptr, &mHandle), "vkCreateImage");

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(mDevice, mHandle, &requirements);
        mMemory = DeviceMemory(
            device, requirements.size, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);
        checkVk(vkBindImageMemory(mDevice, mHandle, mMemory.getHandle(), 0), "vkBindImageMemory");

        Buffer staging(device, data.mBytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        staging.write(data.mBytes);

        // Every level in one submit: the levels are already contiguous in the source, so this is one
        // copy per level out of one buffer rather than one upload per level.
        std::vector<VkBufferImageCopy> regions;
        regions.reserve(levels);
        for (std::uint32_t level = 0; level < levels; ++level)
            regions.push_back(VkBufferImageCopy{
                .bufferOffset = data.mLevels[level].mOffset,
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 },
                .imageExtent = { data.mLevels[level].mWidth, data.mLevels[level].mHeight, 1 },
            });

        const VkImageSubresourceRange whole{ VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 };

        // **Recorded rather than submitted.** A cell brings hundreds of these and the queue is asked
        // once for all of them; the image is left where a sampler expects it, so nothing recorded
        // afterwards has to know this one happened.
        const VkCommandBuffer commands = batch.getCommands();
        {
            const auto barrier
                = [&](VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                      VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
                      const VkImageMemoryBarrier2 image{
                          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                          .srcStageMask = srcStage,
                          .srcAccessMask = srcAccess,
                          .dstStageMask = dstStage,
                          .dstAccessMask = dstAccess,
                          .oldLayout = from,
                          .newLayout = to,
                          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                          .image = mHandle,
                          .subresourceRange = whole,
                      };
                      const VkDependencyInfo dependency{
                          .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                          .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &image,
                      };
                      vkCmdPipelineBarrier2(commands, &dependency);
                  };

            barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

            vkCmdCopyBufferToImage(commands, staging.getHandle(), mHandle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                static_cast<std::uint32_t>(regions.size()), regions.data());

            barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }

        batch.keep(std::move(staging));

        const VkImageViewCreateInfo view{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = mHandle,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .subresourceRange = whole,
        };
        checkVk(vkCreateImageView(mDevice, &view, nullptr, &mView), "vkCreateImageView");

        device.setName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<std::uint64_t>(mHandle), name);
        device.setName(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<std::uint64_t>(mView), name);
    }

    Texture::~Texture()
    {
        destroy();
    }

    Texture::Texture(Texture&& other) noexcept
        : mDevice(other.mDevice)
        , mHandle(std::exchange(other.mHandle, VK_NULL_HANDLE))
        , mView(std::exchange(other.mView, VK_NULL_HANDLE))
        , mMemory(std::move(other.mMemory))
        , mBytes(other.mBytes)
    {
    }

    Texture& Texture::operator=(Texture&& other) noexcept
    {
        if (this != &other)
        {
            destroy();
            mDevice = other.mDevice;
            mHandle = std::exchange(other.mHandle, VK_NULL_HANDLE);
            mView = std::exchange(other.mView, VK_NULL_HANDLE);
            mMemory = std::move(other.mMemory);
            mBytes = other.mBytes;
        }
        return *this;
    }

    void Texture::destroy()
    {
        if (mView != VK_NULL_HANDLE)
            vkDestroyImageView(mDevice, mView, nullptr);
        if (mHandle != VK_NULL_HANDLE)
            vkDestroyImage(mDevice, mHandle, nullptr);
        mView = VK_NULL_HANDLE;
        mHandle = VK_NULL_HANDLE;
        mMemory = DeviceMemory();
    }

    namespace
    {
        /// Every texture's shading map end to end, and a neutral one wherever there is no estimate.
        ///
        /// **A missing map has to be neutral rather than absent.** A material whose texture would
        /// not load still indexes this buffer, and reading whatever happened to be at that offset is
        /// how the reference implementation came to divide every untextured surface by two.
        constexpr std::size_t sShadingCells = std::size_t{ Shaders::SHADING_EXTENT } * Shaders::SHADING_EXTENT;

        /// Writes each description's shading map into `values` at the slot it names, a stand-in
        /// where it has none.
        void gatherShadingAt(std::span<const TextureData> textures, std::vector<float>& values)
        {
            for (const TextureData& texture : textures)
            {
                const std::size_t at = std::size_t{ texture.mSlot } * sShadingCells;
                if (values.size() < at + sShadingCells)
                    values.resize(at + sShadingCells, 1.0f);

                const std::span<const float> map = texture.mShading;
                if (map.empty())
                {
                    std::fill_n(values.begin() + static_cast<std::ptrdiff_t>(at), sShadingCells, 1.0f);
                    continue;
                }

                assert(map.size() == sShadingCells);
                std::copy(map.begin(), map.end(), values.begin() + static_cast<std::ptrdiff_t>(at));
            }
        }
    }

    TextureArray::TextureArray(
        const Device& device, Batch& batch, std::uint32_t slots, std::span<const TextureData> textures)
        : mDevice(device)
    {
        if (slots > sMaxTextures)
            throw Error("a scene with " + std::to_string(slots) + " textures is past the "
                + std::to_string(sMaxTextures) + " this array holds");

        // **The shading table exists from here.** An array with no textures in it is asked for none
        // and would grow to nothing, and the descriptor the shader declares would be bound to a null
        // handle — which is undefined at the dispatch and cost this renderer a device.
        growTo(mShading, device, 0, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

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
        constexpr std::uint32_t count = sMaxTextures;
        mLayout = makeLayout(device);

        const VkDescriptorPoolSize size{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, count };
        const VkDescriptorPoolCreateInfo describePool{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = 1,
            .poolSizeCount = 1,
            .pPoolSizes = &size,
        };
        checkVk(vkCreateDescriptorPool(device.getHandle(), &describePool, nullptr, &mPool), "vkCreateDescriptorPool");

        // What the layout left open: the array is declared at its maximum and allocated at the
        // scene's, so the descriptors paid for are the ones a cell put in it.
        const VkDescriptorSetVariableDescriptorCountAllocateInfo variable{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
            .descriptorSetCount = 1,
            .pDescriptorCounts = &count,
        };
        const VkDescriptorSetAllocateInfo allocate{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .pNext = &variable,
            .descriptorPool = mPool,
            .descriptorSetCount = 1,
            .pSetLayouts = &mLayout,
        };
        checkVk(vkAllocateDescriptorSets(device.getHandle(), &allocate, &mSet), "vkAllocateDescriptorSets");

        // **Sized to the table before anything is written into it**, so a description lands in the
        // slot it names whatever sits either side of it. Every entry starts holding no image and no
        // estimate, which is what a free slot goes on holding: its descriptor is never written, and
        // the binding's `PARTIALLY_BOUND` is what makes that legal for one nothing samples.
        mTextures.resize(slots);
        mShadingValues.assign(std::size_t{ slots } * sShadingCells, 1.0f);

        write(batch, textures);

        // **A scene with no textures still binds the shading buffer**, and `write` has nothing to do
        // for one — so the neutral map that stands in for an empty array is made here rather than
        // inside a path that returns before it.
        growShading();
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

    void TextureArray::write(Batch& batch, std::span<const TextureData> arrived)
    {
        if (arrived.empty())
            return;

        for (const TextureData& texture : arrived)
        {
            reserveSlot(texture.mSlot);

            // Assigned rather than emplaced, so whatever the slot held is destroyed here — after its
            // descriptor has stopped being the one bound and before the new one replaces it.
            mTextures[texture.mSlot] = Texture(mDevice, batch, texture, "texture " + std::to_string(texture.mSlot));
        }

        gatherShadingAt(arrived, mShadingValues);
        reshade(arrived);
        describe(arrived);
    }

    void TextureArray::drop(std::span<const std::uint32_t> slots)
    {
        for (const std::uint32_t slot : slots)
        {
            // A slot this array never held: a scene can add a texture and sweep it in the same
            // window, before anything was handed over to upload it.
            if (slot >= mTextures.size())
                continue;

            // Assigned rather than erased, so the image is destroyed and the slot stays where it is.
            mTextures[slot] = Texture();
        }
    }

    void TextureArray::describe(std::span<const TextureData> arrived)
    {
        if (arrived.empty())
            return;

        // One write per slot rather than one over a range: the arrivals are wherever the scene's
        // free list put them, and a run is no longer what they are.
        std::vector<VkDescriptorImageInfo> images;
        std::vector<VkWriteDescriptorSet> writes;
        images.reserve(arrived.size());
        writes.reserve(arrived.size());

        for (const TextureData& texture : arrived)
            images.push_back(VkDescriptorImageInfo{
                .sampler = mSampler,
                .imageView = mTextures[texture.mSlot].getView(),
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            });

        for (std::size_t at = 0; at < arrived.size(); ++at)
            writes.push_back(VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = mSet,
                .dstBinding = 0,
                .dstArrayElement = arrived[at].mSlot,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &images[at],
            });

        vkUpdateDescriptorSets(
            mDevice.getHandle(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    void TextureArray::reshade(std::span<const TextureData> arrived)
    {
        if (growShading())
            return;

        for (const TextureData& texture : arrived)
            mShading.writeAt(std::size_t{ texture.mSlot } * sShadingCells * sizeof(float),
                std::span<const float>(mShadingValues)
                    .subspan(std::size_t{ texture.mSlot } * sShadingCells, sShadingCells));
    }

    bool TextureArray::growShading()
    {
        // One texture's worth even for a scene with none: a buffer of nothing is not a legal thing
        // to make, and the descriptor is bound either way.
        if (mShadingValues.empty())
            mShadingValues.assign(sShadingCells, 1.0f);

        const std::span<const float> values(mShadingValues);

        // **Grown in blocks, and only then rewritten whole.** A buffer sized exactly to the scene
        // would be made again for every texture that arrives, which is the spike this exists to
        // remove; a block of slack turns that into one write per arrival, most of the time.
        if (mShading.getSize() >= values.size_bytes())
            return false;

        constexpr std::size_t slack = 128 * sShadingCells;
        const std::size_t room = ((values.size() + slack - 1) / slack) * slack;

        // **`growTo` and not a constructor, because an array with no textures in it still has this
        // descriptor.** Sized to nought it was never made at all, and the null handle reached
        // `vkCmdDispatch` — undefined, and the device this renderer lost.
        growTo(mShading, mDevice, room * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        mShading.write(values);
        return true;
    }

    TextureArray::~TextureArray()
    {
        if (mPool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(mDevice.getHandle(), mPool, nullptr);
        if (mLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(mDevice.getHandle(), mLayout, nullptr);
        if (mSampler != VK_NULL_HANDLE)
            vkDestroySampler(mDevice.getHandle(), mSampler, nullptr);
    }

    VkDeviceSize TextureArray::getBytes() const
    {
        VkDeviceSize total = 0;
        for (const Texture& texture : mTextures)
            total += texture.getBytes();
        return total;
    }
}
