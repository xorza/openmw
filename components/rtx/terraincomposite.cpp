#include "terraincomposite.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>

#include <osg/Vec3f>

#include "shadingmap.hpp"
#include "srgb.hpp"
#include "texelreader.hpp"

namespace Rtx
{
    namespace
    {
        /// The lower of the two texels a bilinear tap falls between along one axis, and how far it
        /// is towards the upper one.
        ///
        /// **Texel centres sit at half-integers**, so the footprint starts half a texel back. That is
        /// the one rule the mask lookup and the diffuse fetch below have to agree on: half a texel of
        /// drift between them puts a ground type's blend somewhere its mask never said.
        struct Between
        {
            int mLow = 0;
            float mAcross = 0.0f;
        };

        Between between(float at, std::uint32_t size)
        {
            const float texel = at * static_cast<float>(size) - 0.5f;
            const auto low = static_cast<int>(std::floor(texel));

            return Between{ low, texel - static_cast<float>(low) };
        }

        /// How much of a layer shows at a point of the chunk — the shader's `maskWeight`, in C++.
        ///
        /// **Clamped at the grid's edges where every other texture in this scene repeats.** A mask
        /// is ten texels across and states the whole chunk; wrapping it would blend the far side of
        /// the chunk into the near one, which is a ground type appearing where it is not.
        float maskWeight(const CompositeLayer& layer, float u, float v)
        {
            // A chunk of one ground type is given no mask at all: there is nothing to blend against.
            if (layer.mMaskWidth == 0 || layer.mMaskHeight == 0)
                return 1.0f;

            assert(layer.mMask.size() == std::size_t{ layer.mMaskWidth } * layer.mMaskHeight);

            const float atU = u * layer.mMaskTransform.x() + layer.mMaskTransform.z();
            const float atV = v * layer.mMaskTransform.y() + layer.mMaskTransform.w();

            const Between across = between(atU, layer.mMaskWidth);
            const Between down = between(atV, layer.mMaskHeight);

            const auto edge = [](int at, std::uint32_t bound) {
                return static_cast<std::uint32_t>(std::clamp(at, 0, static_cast<int>(bound) - 1));
            };

            const std::uint32_t firstX = edge(across.mLow, layer.mMaskWidth);
            const std::uint32_t secondX = edge(across.mLow + 1, layer.mMaskWidth);
            const std::uint32_t firstY = edge(down.mLow, layer.mMaskHeight);
            const std::uint32_t secondY = edge(down.mLow + 1, layer.mMaskHeight);

            const auto cell = [&](std::uint32_t column, std::uint32_t row) {
                return layer.mMask[std::size_t{ row } * layer.mMaskWidth + column];
            };

            const float top = std::lerp(cell(firstX, firstY), cell(secondX, firstY), across.mAcross);
            const float bottom = std::lerp(cell(firstX, secondY), cell(secondX, secondY), across.mAcross);

            return std::lerp(top, bottom, down.mAcross);
        }

        /// One level of a layer's diffuse, decoded to linear once.
        ///
        /// **Decoded up front rather than a block per tap.** The level a bake reads is the one whose
        /// texels are the size of one composite texel, so for ground tiling sixty times across a
        /// chunk it is a handful of texels square — while the composite takes a quarter of a million
        /// samples from it. Reading a compressed block at every tap made a chunk cost 56 ms; reading
        /// each level once makes every tap an array lookup and changes not one texel of the answer.
        struct Decoded
        {
            std::vector<osg::Vec3f> mTexels;
            std::uint32_t mWidth = 0;
            std::uint32_t mHeight = 0;

            bool isEmpty() const { return mTexels.empty(); }
        };

        Decoded decodeLevel(const TextureData& texture, const MipLevel& level)
        {
            Decoded made;
            made.mWidth = level.mWidth;
            made.mHeight = level.mHeight;
            made.mTexels.reserve(std::size_t{ level.mWidth } * level.mHeight);

            const bool encoded = isSrgb(texture.mFormat);
            for (std::uint32_t y = 0; y < level.mHeight; ++y)
                for (std::uint32_t x = 0; x < level.mWidth; ++x)
                {
                    const osg::Vec3f stored = texelAt(texture, level, x, y);
                    made.mTexels.push_back(encoded ? toLinear(stored) : stored);
                }

            return made;
        }

        /// One layer's diffuse, reduced to the two levels the whole bake will read and how far it
        /// sits between them.
        struct Sampled
        {
            Decoded mFine;
            Decoded mCoarse;
            float mBetween = 0.0f;
        };

        /// One decoded level at a point, bilinear and repeating.
        ///
        /// Repeating because the ground tiles and the one sampler every texture in this scene shares
        /// repeats with it — a bake that clamped would smear the last row of a ground texture across
        /// the whole of the chunk it runs off the edge of.
        osg::Vec3f bilinear(const Decoded& level, float u, float v)
        {
            const Between across = between(u, level.mWidth);
            const Between down = between(v, level.mHeight);

            const auto wrap = [](int at, std::uint32_t bound) {
                const auto side = static_cast<int>(bound);
                return static_cast<std::uint32_t>((at % side + side) % side);
            };

            const std::uint32_t firstX = wrap(across.mLow, level.mWidth);
            const std::uint32_t secondX = wrap(across.mLow + 1, level.mWidth);
            const std::uint32_t firstY = wrap(down.mLow, level.mHeight);
            const std::uint32_t secondY = wrap(down.mLow + 1, level.mHeight);

            const auto fetch = [&](std::uint32_t column, std::uint32_t row) -> const osg::Vec3f& {
                return level.mTexels[std::size_t{ row } * level.mWidth + column];
            };

            const osg::Vec3f top
                = fetch(firstX, firstY) * (1.0f - across.mAcross) + fetch(secondX, firstY) * across.mAcross;
            const osg::Vec3f bottom
                = fetch(firstX, secondY) * (1.0f - across.mAcross) + fetch(secondX, secondY) * across.mAcross;

            return top * (1.0f - down.mAcross) + bottom * down.mAcross;
        }

        /// A prepared layer at a point, trilinear between the two levels it kept.
        osg::Vec3f sampleLinear(const Sampled& layer, float u, float v)
        {
            if (layer.mFine.isEmpty())
                return osg::Vec3f();

            if (layer.mBetween <= 0.0f || layer.mCoarse.isEmpty())
                return bilinear(layer.mFine, u, v);

            return bilinear(layer.mFine, u, v) * (1.0f - layer.mBetween)
                + bilinear(layer.mCoarse, u, v) * layer.mBetween;
        }

        /// Decodes the two levels of a layer's diffuse whose texels are the size of a composite
        /// texel, which is all of it the bake will ever read.
        ///
        /// **A point sample of a tiling texture is noise at this scale.** A chunk several cells
        /// across tiles its ground hundreds of times, so one output texel covers hundreds of input
        /// ones; reading the finest level picks an arbitrary one of them and the chunk comes out
        /// speckled rather than the colour the ground averages to.
        ///
        /// The level is constant across the whole composite because the transform is, which is what
        /// makes decoding it once possible at all.
        Sampled prepare(const CompositeLayer& layer, std::uint32_t extent)
        {
            const TextureData& texture = layer.mDiffuse;
            if (texture.mLevels.empty())
                return Sampled{};

            const MipLevel& finest = texture.mLevels.front();
            const float texelsAcross = std::abs(layer.mDiffuseTransform.x()) * static_cast<float>(finest.mWidth);
            const float texelsDown = std::abs(layer.mDiffuseTransform.y()) * static_cast<float>(finest.mHeight);

            // Never below one texel a composite texel: a composite finer than the ground it is made
            // of magnifies, and a negative level is not a level.
            const float footprint
                = std::max({ texelsAcross, texelsDown, static_cast<float>(extent) }) / static_cast<float>(extent);

            const auto deepest = static_cast<std::uint32_t>(texture.mLevels.size() - 1);
            const float wanted = std::clamp(std::log2(footprint), 0.0f, static_cast<float>(deepest));
            const auto fine = static_cast<std::uint32_t>(wanted);
            const std::uint32_t coarse = std::min(fine + 1, deepest);

            Sampled made;
            made.mFine = decodeLevel(texture, texture.mLevels[fine]);
            made.mBetween = wanted - static_cast<float>(fine);

            if (made.mBetween > 0.0f && coarse != fine)
                made.mCoarse = decodeLevel(texture, texture.mLevels[coarse]);

            return made;
        }

        std::byte encodeByte(float linear)
        {
            return static_cast<std::byte>(std::lround(toEncoded(linear) * 255.0f));
        }
    }

    TerrainComposite::TerrainComposite(std::span<const CompositeLayer> layers, std::uint32_t extent, float delight)
        : mExtent(extent)
    {
        assert(!layers.empty() && "a composite of no layers is a chunk with no ground at all");
        assert(extent > 0 && std::has_single_bit(extent) && "a composite extent the chain cannot halve to one texel");

        std::vector<Sampled> grounds;
        grounds.reserve(layers.size());
        for (const CompositeLayer& layer : layers)
            grounds.push_back(prepare(layer, extent));

        // The sum, in light, one entry a texel of the finest level.
        std::vector<osg::Vec3f> light(std::size_t{ extent } * extent);

        for (std::uint32_t y = 0; y < extent; ++y)
        {
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(extent);

            for (std::uint32_t x = 0; x < extent; ++x)
            {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(extent);

                osg::Vec3f sum;
                for (std::size_t index = 0; index < layers.size(); ++index)
                {
                    const CompositeLayer& layer = layers[index];
                    const float showing = maskWeight(layer, u, v);
                    if (showing <= 0.0f)
                        continue;

                    const osg::Vec4f& transform = layer.mDiffuseTransform;
                    const float atU = u * transform.x() + transform.z();
                    const float atV = v * transform.y() + transform.w();

                    osg::Vec3f colour = sampleLinear(grounds[index], atU, atV);

                    // **The layer's own texel, at the layer's own tiled coordinates.** The estimate
                    // repeats with the texture and this is the last point at which that tiling is
                    // still known — dividing the finished sum, at the chunk's coordinates, would be
                    // correcting a texture that no longer exists by a map that never described it.
                    if (delight > 0.0f && !layer.mShading.empty())
                        colour /= std::lerp(1.0f, paintedLight(layer.mShading, atU, atV), delight);

                    sum += colour * showing;
                }

                light[std::size_t{ y } * extent + x] = sum;
            }
        }

        buildChain(light);
    }

    void TerrainComposite::buildChain(std::vector<osg::Vec3f>& light)
    {
        const auto count = static_cast<std::uint32_t>(std::countr_zero(mExtent)) + 1;

        std::size_t total = 0;
        for (std::uint32_t at = 0, side = mExtent; at < count; ++at, side /= 2)
            total += std::size_t{ side } * side * 4;

        mBytes.resize(total);
        mLevels.reserve(count);

        std::vector<osg::Vec3f> coarser;
        std::uint32_t offset = 0;
        for (std::uint32_t at = 0, side = mExtent; at < count; ++at, side /= 2)
        {
            if (at > 0)
            {
                // Box-filtered in light for the reason the blend above is summed in it, and built
                // here rather than left to whatever the file carried: a composite has no file.
                const std::uint32_t finer = side * 2;
                coarser.assign(std::size_t{ side } * side, osg::Vec3f());

                for (std::uint32_t y = 0; y < side; ++y)
                    for (std::uint32_t x = 0; x < side; ++x)
                    {
                        const std::size_t from = std::size_t{ y } * 2 * finer + std::size_t{ x } * 2;
                        coarser[std::size_t{ y } * side + x]
                            = (light[from] + light[from + 1] + light[from + finer] + light[from + finer + 1]) * 0.25f;
                    }

                light.swap(coarser);
            }

            mLevels.push_back(MipLevel{ offset, side, side });

            for (std::size_t texel = 0; texel < std::size_t{ side } * side; ++texel)
            {
                const osg::Vec3f& colour = light[texel];
                std::byte* into = mBytes.data() + offset + texel * 4;

                into[0] = encodeByte(colour.x());
                into[1] = encodeByte(colour.y());
                into[2] = encodeByte(colour.z());
                into[3] = std::byte{ 255 };
            }

            offset += side * side * 4;
        }
    }

    TextureData TerrainComposite::describe() const
    {
        // **Neutral, and one grid shared by every composite there will ever be.** A texture with no
        // map at all reads whatever the array's stand-in holds, and there is nothing left for a real
        // one to say: the light painted into the ground came off per tile during the bake, which is
        // the only place the tiling was still known.
        static const ShadingMap sNeutral;

        return TextureData{
            .mFormat = TextureFormat::Rgba8Srgb,
            .mWidth = mExtent,
            .mHeight = mExtent,
            .mBytes = mBytes,
            .mLevels = mLevels,
            .mShading = sNeutral.getValues(),
        };
    }
}
