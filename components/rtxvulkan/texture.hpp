#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtx/texturedata.hpp>

#include <memory>

#include "image.hpp"
#include "setlayout.hpp"

namespace Rtx
{
    class Batch;
    class Device;
    class Graveyard;

    /// A sampled image on the GPU, the levels a content file brought for it, and the light the
    /// file already had painted into it.
    ///
    /// **Two `Image`s and what was uploaded into them.** What a texture adds to an image is the
    /// upload and the size of it; everything else — the allocation, the view, the barriers — is what
    /// an image already is. The second is the shading map, `SHADING_EXTENT` squared, which travels
    /// with the texture because it is a fact about the texture: it is measured on it, it is read at
    /// the texture's own coordinates, and it goes when the texture goes.
    class Texture
    {
    public:
        /// A slot with nothing in it yet, which is what the array holds while it is being filled.
        Texture() = default;

        Texture(const Device& device, Batch& batch, const TextureData& data, std::string_view name);

        Texture(const Texture&) = delete;
        Texture& operator=(const Texture&) = delete;
        Texture(Texture&&) noexcept = default;
        Texture& operator=(Texture&&) noexcept = default;

        /// What a sampler reads, or nothing where the slot holds no texture.
        VkImageView getView() const { return mImage == nullptr ? VK_NULL_HANDLE : mImage->getView(); }

        /// The shading map beside it, likewise.
        VkImageView getShadingView() const { return mShading == nullptr ? VK_NULL_HANDLE : mShading->getView(); }

        /// The size of the data uploaded, the map's included, which for a block-compressed image is
        /// what it occupies.
        VkDeviceSize getBytes() const { return mBytes; }

    private:
        /// **By pointer, because `Image` is not movable** and a texture is: the array holds them in
        /// a vector, and a slot given back is buried under the frame that may still be reading it.
        std::unique_ptr<Image> mImage;
        std::unique_ptr<Image> mShading;

        VkDeviceSize mBytes = 0;
    };

    /// What a texture array stands: how many of its slots hold a texture, and what those come to.
    ///
    /// **One walk for both, so the two cannot disagree about which slots they counted.** A length
    /// and a sum over what is live are different questions, and a report answering one of each is
    /// what `SceneStats::mTextureCount` says this replaced.
    struct TexturesHeld
    {
        std::uint32_t mCount = 0;
        VkDeviceSize mBytes = 0;
    };

    /// Every texture a scene uses, in one descriptor array a shader indexes by material, and every
    /// texture's shading map in a second array beside it at the same slot.
    ///
    /// A separate set from the per-frame one: this is written once and bound for the run, while the
    /// other is pushed every frame. A bindless array cannot be a push descriptor anyway — there is
    /// no pushing four thousand of them per frame.
    ///
    /// **The maps are an array and not a buffer**, because a map is a grid the texture unit filters:
    /// one fetch where a shader reading one out of a buffer paid four loads and the wrap by hand,
    /// half the memory, and no table to rewrite whole when it grows. Measured, the loads cost
    /// nothing the trace can see, so this is the shape and not a saving. And an array of their own
    /// rather than slots among the textures, for the reason `texturearray.glsl` gives.
    class TextureArray
    {
    public:
        /// An array of `slots` textures, with `textures` written into the slots they name.
        ///
        /// **The length is the scene's table and not what was described**, because a slot the scene
        /// has given back is described by nobody and still sits between two that are: sizing to the
        /// descriptions would put every texture above it one place too low. It also keeps `getCount`
        /// equal to the table an uploader compares against, so a trailing free slot does not read as
        /// a scene this array has never seen.
        ///
        /// `textures` may be empty. The shader declares the array unsized and is told no count at
        /// all: what keeps every read in range is that the array is as long as the scene's table,
        /// and a slot nothing describes is one no material names — which is what
        /// `descriptorBindingPartiallyBound` is required for.
        ///
        /// `graveyard` is where what this displaces goes: nothing at construction, but the path is
        /// one.
        TextureArray(const Device& device, Batch& batch, std::uint32_t slots, std::span<const TextureData> textures,
            Graveyard& graveyard);
        ~TextureArray();

        /// Writes each of `arrived` into the slot it names, leaving every other texture alone.
        ///
        /// **This is why the set is allocated at the maximum rather than at the scene's count.**
        /// A cell arriving, or an actor walking into view with a body texture nobody has worn yet,
        /// used to mean the whole array made again — 327 images re-uploaded, measured at 150 to 225
        /// milliseconds, against 12 for every acceleration structure in the scene.
        ///
        /// **By slot and not by appending**, because a slot a departing cell freed is taken over
        /// wherever it sits. A slot at the end grows the array; one inside it replaces what was
        /// there, and the image that was there goes when it is replaced and not when it was freed —
        /// so no descriptor ever names an image that has been destroyed.
        ///
        /// What a slot held before goes to `graveyard`: a frame in flight may be reading it.
        void write(Batch& batch, std::span<const TextureData> arrived, Graveyard& graveyard);

        /// Destroys the images of `slots`, leaving the slots themselves where they are.
        ///
        /// **The descriptors are left naming what has gone**, which the binding's
        /// `VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT` makes legal: a descriptor that is not
        /// dynamically used need not be valid, and no live material names a freed slot. Writing a
        /// stand-in over each would cost a descriptor write per slot to change nothing a shader can
        /// observe.
        ///
        /// The array does not shrink even where the slots are its last: `getCount` is where an
        /// append begins and the scene's table has not shrunk either.
        void drop(std::span<const std::uint32_t> slots, Graveyard& graveyard);

        TextureArray(const TextureArray&) = delete;
        TextureArray& operator=(const TextureArray&) = delete;

        VkDescriptorSetLayout getLayout() const { return mLayout.getHandle(); }
        VkDescriptorSet getSet() const { return mSet; }

        /// How long the array is, which is where an append begins and what an uploader compares a
        /// scene's table against. Not how many textures there are: see `getHeld`.
        std::uint32_t getCount() const { return static_cast<std::uint32_t>(mTextures.size()); }

        /// What the array actually stands. A slot the scene gave back holds nothing and costs
        /// nothing, and neither is counted here.
        TexturesHeld getHeld() const;

    private:
        /// Writes the descriptors for the slots `arrived` names, the texture's and its map's.
        void describe(std::span<const TextureData> arrived);

        /// Grows the array to reach `slot`, and refuses one past what the binding holds.
        void reserveSlot(std::uint32_t slot);

        const Device& mDevice;

        /// Indexed by slot. A slot the scene has freed holds nothing until something takes it over —
        /// `drop` buries the image it had, and the descriptor is left naming what has gone for the
        /// reason `drop` gives.
        std::vector<Texture> mTextures;

        VkSampler mSampler = VK_NULL_HANDLE;
        SetLayout mLayout;
        VkDescriptorPool mPool = VK_NULL_HANDLE;
        VkDescriptorSet mSet = VK_NULL_HANDLE;
    };
}
