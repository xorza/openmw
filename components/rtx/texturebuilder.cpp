#include "texturebuilder.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <osg/Image>

#include <components/debug/debuglog.hpp>
#include <components/resource/imagemanager.hpp>

#include "alphaimage.hpp"
#include "compositequeue.hpp"
#include "error.hpp"
#include "scenedesc.hpp"
#include "shadingmap.hpp"
#include "texels.hpp"

namespace Rtx
{
    osg::ref_ptr<const osg::Image> openImage(Resource::ImageManager& images, const VFS::Path::NormalizedView path)
    {
        try
        {
            return images.getImage(path);
        }
        catch (const std::exception&)
        {
            return nullptr;
        }
    }

    namespace
    {
        /// What the content is, as one of the formats this renderer uploads.
        ///
        /// Nothing where the file is something else, so the caller says so with the file's name in
        /// the message. `TextureFormat` is why every case is sRGB.
        std::optional<TextureFormat> toTextureFormat(ImageFormat format)
        {
            switch (format)
            {
                // Both DXT1 spellings land on the format that reads the alpha bit: every mask in the
                // game is a punch-through BC1 block, and almost none of Morrowind's files set
                // `DDPF_ALPHAPIXELS`, so believing the header would leave every canopy a solid card.
                case ImageFormat::Bc1:
                    return TextureFormat::Bc1RgbaSrgb;
                case ImageFormat::Bc2:
                    return TextureFormat::Bc2Srgb;
                case ImageFormat::Bc3:
                    return TextureFormat::Bc3Srgb;

                // **Not every file the game ships is a block.** The sky's cloud decks are plain
                // 32-bit `DDPF_RGB`, which is what a texture painted for a full-screen dome would
                // be, and taking only the compressed formats would draw every weather's clouds grey.
                case ImageFormat::Rgba8:
                    return TextureFormat::Rgba8Srgb;
                case ImageFormat::Bgra8:
                    return TextureFormat::Bgra8Srgb;

                // Three-channel and single-channel spellings are refused deliberately: uploading
                // one would need the missing channels written in, which means owning a buffer, and
                // nothing this renderer reads stores a texture without them.
                case ImageFormat::Rgb8:
                case ImageFormat::Luminance:
                case ImageFormat::LuminanceAlpha:
                case ImageFormat::Unnamed:
                    return std::nullopt;
            }

            return std::nullopt;
        }

        /// What a texture that could not be read is drawn as: mid grey and not magenta, because a
        /// live graph's unreadable textures are mostly things that were never files, and
        /// `getUnreadable` already reports them. One opaque BC1 block with both endpoints the same
        /// grey.
        TextureData standIn(std::vector<MipLevel>& levels)
        {
            // 0x8410 is RGB565 for (16, 16, 16) out of (31, 63, 31) — a touch above half, which is
            // mid grey once the sRGB curve is undone.
            static constexpr std::array<std::byte, 8> sBlock{ std::byte{ 0x10 }, std::byte{ 0x84 }, std::byte{ 0x10 },
                std::byte{ 0x84 }, std::byte{}, std::byte{}, std::byte{}, std::byte{} };

            const std::size_t first = levels.size();
            levels.push_back(MipLevel{ 0, 4, 4 });

            return TextureData{
                .mFormat = TextureFormat::Bc1RgbaSrgb,
                .mWidth = 4,
                .mHeight = 4,
                .mBytes = sBlock,
                .mLevels = std::span<const MipLevel>(levels).subspan(first, 1),
                .mName = "unreadable",
            };
        }
    }

    TextureData describeImage(const osg::Image& image, std::vector<MipLevel>& levels)
    {
        const std::optional<TextureFormat> format = toTextureFormat(readFormat(image));
        if (!format.has_value())
            throw Error("texture \"" + image.getFileName() + "\" is pixel format "
                + std::to_string(image.getPixelFormat()) + ", which is not one this renderer uploads");

        const auto width = static_cast<std::uint32_t>(image.s());
        const auto height = static_cast<std::uint32_t>(image.t());

        const std::size_t first = levels.size();
        const unsigned int count = image.getNumMipmapLevels();
        for (unsigned int level = 0; level < count; ++level)
            levels.push_back(MipLevel{
                .mOffset = image.getMipmapOffset(level),
                .mWidth = std::max(width >> level, 1u),
                .mHeight = std::max(height >> level, 1u),
            });

        return TextureData{
            .mFormat = *format,
            .mWidth = width,
            .mHeight = height,
            .mBytes
            = std::span(reinterpret_cast<const std::byte*>(image.data()), image.getTotalSizeInBytesIncludingMipmaps()),
            .mLevels = std::span<const MipLevel>(levels).subspan(first, count),
            .mName = image.getFileName(),
        };
    }

    void SceneTextures::describeAll(const SceneDesc& scene, Resource::ImageManager& images,
        const CompositeQueue* composites, const TextureReadings* readings)
    {
        mEverything.resize(scene.textures().getPaths().size());
        std::iota(mEverything.begin(), mEverything.end(), Index{ 0 });

        describe(scene, images, mEverything, composites, readings);
    }

    void SceneTextures::describe(const SceneDesc& scene, Resource::ImageManager& images, std::span<const Index> slots,
        const CompositeQueue* composites, const TextureReadings* readings)
    {
        mLevels.clear();
        mDescriptions.clear();
        mKept.clear();
        mSpriteLights.reset();
        mChains.reset();
        mUnreadable = 0;

        mKept.reserve(slots.size());

        for (const Index slot : slots)
        {
            // **A free slot is not a texture.** `SceneDesc` empties one the last thing naming it
            // gave back and leaves it in the table until something takes it over; describing it
            // would build an image, a shading map and a descriptor write for a slot no material can
            // reach — and count it as a texture that arrived.
            if (scene.textures().isFree(slot))
                continue;

            // A slot this renderer made rather than opened has no file to be asked for, and the
            // entry still has to exist because the description below is built from it. A composite
            // the queue has not finished is passed over the same way.

            osg::ref_ptr<const osg::Image> image;
            Index light = sNoIndex;

            const std::string& baked = scene.textures().getBaked()[slot];
            if (baked.empty())
                image = openImage(images, scene.textures().getPaths()[slot]);
            else if (const std::optional<VFS::Path::Normalized> source = SpriteLightMap::sourceOf(baked))
            {
                // **Baked from the sprite texture's alpha, here, because here is where a file is
                // opened for upload.** The source's own description is transient — its levels go
                // in a table thrown away with it — since nothing reaches the source through this
                // slot; the emitter names the source by a slot of its own.
                if (const osg::ref_ptr<const osg::Image> sprite = openImage(images, *source))
                {
                    mSourceLevels.clear();
                    try
                    {
                        TextureData painted = describeImage(*sprite, mSourceLevels);

                        // **The same chain the sprite's own slot gets**, because a bake is read at
                        // whatever level the ray can resolve and a source with one level would bake
                        // one answer for every distance.
                        mSourceChain.build(painted);
                        if (!mSourceChain.isEmpty())
                            painted = mSourceChain.describe();

                        mSourceAlpha.build(painted);
                        if (!mSourceAlpha.isEmpty())
                        {
                            SpriteLightMap& bake = mSpriteLights.next();
                            bake.build(mSourceAlpha);

                            light = static_cast<Index>(mSpriteLights.keep());
                        }
                    }
                    catch (const Error&)
                    {
                        // A source in a format this renderer does not upload is a bake with no
                        // alpha to read, and it gets the stand-in below like the source itself.
                    }
                }
            }

            mKept.push_back(Kept{ .mSlot = slot, .mLight = light, .mImage = std::move(image) });
        }

        // **Reserved before anything points into it, and that is what makes the spans safe.** Every
        // description spans this one table, so it must not grow while they are being taken — and
        // every level count is known before the first description is built. A table kept from the
        // last arrival is usually large enough already, and then this asks for nothing.
        std::size_t levels = 0;
        for (const Kept& kept : mKept)
            levels += kept.mImage != nullptr ? kept.mImage->getNumMipmapLevels() : 1u;
        mLevels.reserve(levels);

        // What the assertion below is taken against: the reserve and the fill agree by argument
        // through branches that push a different number of levels each, and a growth is the failure.
        [[maybe_unused]] const std::size_t reserved = mLevels.capacity();

        mDescriptions.reserve(mKept.size());
        for (const Kept& kept : mKept)
        {
            std::optional<TextureData> described;
            if (kept.mImage != nullptr)
            {
                try
                {
                    described = describeImage(*kept.mImage, mLevels);

                    // **What was read ahead of the frame is taken, and the rest read here.** A
                    // reading carries the chain the file did not have and the shading estimate;
                    // both span the reading's own storage, which outlives this describe.
                    const PreparedTexture* read = readings != nullptr ? readings->find(*kept.mImage) : nullptr;
                    if (read != nullptr && read->mReadable)
                    {
                        if (!read->mChain.isEmpty())
                            described = read->mChain.describe();

                        described->mShading = std::span<const float>(read->mShading);
                    }
                    else
                    {
                        // **What the file did not carry, built rather than done without.**
                        // `MipChain` says why almost nothing in the game needs this and why the
                        // rain does.
                        MipChain& chain = mChains.next();
                        chain.build(*described);
                        if (!chain.isEmpty())
                        {
                            described = chain.describe();
                            mChains.keep();
                        }
                    }
                }
                catch (const Error&)
                {
                    described.reset();
                }
            }
            else if (kept.mLight != sNoIndex)
            {
                described = mSpriteLights[kept.mLight].describe();
            }
            else if (const TerrainComposite* baked = composites != nullptr ? composites->find(kept.mSlot) : nullptr)
            {
                described = baked->describe();
            }

            if (!described.has_value())
            {
                ++mUnreadable;

                // Named rather than tallied, because a count says a texture is grey and nothing
                // about which one. Whichever of the two named the slot, or a composite that could
                // not be flattened reports itself as a file with no name.
                const std::string_view baked = scene.textures().getBaked()[kept.mSlot];
                Log(Debug::Warning) << "Texture \""
                                    << (baked.empty() ? scene.textures().getPaths()[kept.mSlot].value() : baked)
                                    << "\" could not be read; drawing the stand-in";

                described = standIn(mLevels);
            }

            described->mSlot = kept.mSlot;
            mDescriptions.push_back(*described);
        }

        assert(mLevels.capacity() == reserved && "the level table grew while descriptions spanned it");

        // After the descriptions, because the estimate reads the bytes they point at, and into one
        // table for the reason the levels are: the spans have to stay put.
        constexpr std::size_t cells = std::size_t{ ShadingMap::sExtent } * ShadingMap::sExtent;
        mShading.resize(mDescriptions.size() * cells);

        for (std::size_t i = 0; i < mDescriptions.size(); ++i)
        {
            const auto into = mShading.begin() + static_cast<std::ptrdiff_t>(i * cells);
            const std::span<const float> own = mDescriptions[i].mShading;

            // **A description that carries its own map keeps it.** A composite says neutral,
            // because the light painted into each ground texture came off per tile in the bake;
            // an estimate made from its bytes instead would take the same light off twice, and
            // read a quarter of a million texels on the frame the composite landed in to do it.
            if (own.empty())
            {
                const ShadingMap map(mDescriptions[i]);
                const std::span<const float> values = map.getValues();
                std::copy(values.begin(), values.end(), into);
                continue;
            }

            assert(own.size() == cells);
            std::copy(own.begin(), own.end(), into);
        }

        for (std::size_t i = 0; i < mDescriptions.size(); ++i)
            mDescriptions[i].mShading = std::span(mShading).subspan(i * cells, cells);
    }
}
