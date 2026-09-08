#include "terraincomposite.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <vector>

#include <osg/Vec3f>

#include "shadingmap.hpp"
#include "srgb.hpp"
#include "texelreader.hpp"

namespace Rtx
{
    namespace
    {
        /// A bilinear tap along one axis: the two rows or columns it falls between, and how far it
        /// is from the first towards the second.
        ///
        /// **Texel centres sit at half-integers**, so the footprint starts half a texel back. That is
        /// the one rule the mask lookup and the diffuse fetch below have to agree on: half a texel of
        /// drift between them puts a ground type's blend somewhere its mask never said.
        struct Tap
        {
            std::uint32_t mFirst = 0;
            std::uint32_t mSecond = 0;
            float mAcross = 0.0f;
        };

        /// Where `at` lands on a grid of `size`, with both neighbours held inside it.
        ///
        /// **Clamped at the grid's edges where every other texture in this scene repeats.** A mask
        /// is a few dozen texels across and states the whole chunk; wrapping it would blend the far
        /// side of the chunk into the near one, which is a ground type appearing where it is not.
        Tap clampedTap(float at, std::uint32_t size)
        {
            const float texel = at * static_cast<float>(size) - 0.5f;
            const auto low = static_cast<int>(std::floor(texel));

            const auto hold
                = [&](int of) { return static_cast<std::uint32_t>(std::clamp(of, 0, static_cast<int>(size) - 1)); };

            return Tap{ hold(low), hold(low + 1), texel - static_cast<float>(low) };
        }

        /// The same, repeating, which is what the one sampler every texture in this scene shares
        /// does — a bake that clamped would smear the last row of a ground texture across the whole
        /// of the chunk it runs off the edge of.
        Tap wrappedTap(float at, std::uint32_t size)
        {
            const float texel = at * static_cast<float>(size) - 0.5f;
            const auto low = static_cast<int>(std::floor(texel));
            const auto side = static_cast<int>(size);

            // One division and a correction rather than the two `%` takes to fold a negative: these
            // coordinates run negative wherever the half-texel step above puts them, and a division
            // is the most expensive instruction in the loop that reaches this.
            const auto hold = [&](int of) {
                const int folded = of % side;
                return static_cast<std::uint32_t>(folded < 0 ? folded + side : folded);
            };

            return Tap{ hold(low), hold(low + 1), texel - static_cast<float>(low) };
        }

        void decodeLevel(const TextureData& texture, const MipLevel& level, DecodedLevel& into)
        {
            into.mWidth = level.mWidth;
            into.mHeight = level.mHeight;

            into.mTexels.clear();
            into.mTexels.reserve(std::size_t{ level.mWidth } * level.mHeight);

            const bool encoded = isSrgb(texture.mFormat);
            for (std::uint32_t y = 0; y < level.mHeight; ++y)
                for (std::uint32_t x = 0; x < level.mWidth; ++x)
                {
                    const osg::Vec3f stored = texelAt(texture, level, x, y);
                    into.mTexels.push_back(encoded ? toLinear(stored) : stored);
                }
        }

        osg::Vec3f sampleAt(const DecodedLevel& level, const Tap& across, const Tap& down)
        {
            const auto fetch = [&](std::uint32_t column, std::uint32_t row) -> const osg::Vec3f& {
                return level.mTexels[std::size_t{ row } * level.mWidth + column];
            };

            const osg::Vec3f top = fetch(across.mFirst, down.mFirst) * (1.0f - across.mAcross)
                + fetch(across.mSecond, down.mFirst) * across.mAcross;
            const osg::Vec3f bottom = fetch(across.mFirst, down.mSecond) * (1.0f - across.mAcross)
                + fetch(across.mSecond, down.mSecond) * across.mAcross;

            return top * (1.0f - down.mAcross) + bottom * down.mAcross;
        }

        /// How much of a layer shows at a point of the chunk — the shader's `maskWeight`, in C++.
        float maskWeight(const CompositeLayer& layer, const Tap& across, const Tap& down)
        {
            const auto cell = [&](std::uint32_t column, std::uint32_t row) {
                return layer.mMask[std::size_t{ row } * layer.mMaskWidth + column];
            };

            const float top
                = std::lerp(cell(across.mFirst, down.mFirst), cell(across.mSecond, down.mFirst), across.mAcross);
            const float bottom
                = std::lerp(cell(across.mFirst, down.mSecond), cell(across.mSecond, down.mSecond), across.mAcross);

            return std::lerp(top, bottom, down.mAcross);
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
        void prepare(const CompositeLayer& layer, std::uint32_t extent, Ground& into)
        {
            into.reuse();

            const TextureData& texture = layer.mDiffuse;
            if (texture.mLevels.empty())
                return;

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

            decodeLevel(texture, texture.mLevels[fine], into.mFine);
            into.mBetween = wanted - static_cast<float>(fine);

            if (into.mBetween > 0.0f && coarse != fine)
                decodeLevel(texture, texture.mLevels[coarse], into.mCoarse);
        }

        /// Marks in `covered` the mask columns that hold anything on either row of `down`, and says
        /// whether any does.
        ///
        /// **What keeps nine ground types from costing nine mask lookups a texel.** A chunk several
        /// cells across carries every ground type in them and each covers a corner of it, so most
        /// of the stack is absent from most of any row of it — and a column pair that is empty is a
        /// weight of exactly nought, which is the case the sum below already drops.
        bool coveredColumns(const CompositeLayer& layer, const Tap& down, std::vector<std::uint8_t>& covered)
        {
            covered.resize(layer.mMaskWidth);

            const float* first = layer.mMask.data() + std::size_t{ down.mFirst } * layer.mMaskWidth;
            const float* second = layer.mMask.data() + std::size_t{ down.mSecond } * layer.mMaskWidth;

            std::uint8_t any = 0;
            for (std::uint32_t column = 0; column < layer.mMaskWidth; ++column)
            {
                const auto holds = static_cast<std::uint8_t>(first[column] != 0.0f || second[column] != 0.0f);
                covered[column] = holds;
                any |= holds;
            }

            return any != 0;
        }

        /// One channel of light, as the byte a backend uploads.
        ///
        /// **Rounded here rather than through `std::lround`**, which is a libm call no compiler
        /// inlines and which a chain reaches a million times. `toEncoded` clamps to the unit range,
        /// so the value is inside `[0, 255]` — where the whole part and the remainder are both
        /// exact, and the two roundings agree bit for bit.
        std::byte encodeByte(float linear)
        {
            const float value = toEncoded(linear) * 255.0f;
            const auto whole = static_cast<int>(value);

            return static_cast<std::byte>(value - static_cast<float>(whole) >= 0.5f ? whole + 1 : whole);
        }
    }

    TerrainComposite::TerrainComposite(
        std::span<const CompositeLayer> layers, std::uint32_t extent, float delight, CompositeScratch& scratch)
    {
        assert(!layers.empty() && "a composite of no layers is a chunk with no ground at all");
        assert(extent > 0 && std::has_single_bit(extent) && "a composite extent the chain cannot halve to one texel");

        // **Grown to the deepest stack met and never shrunk**, so a ground decodes into the buffers
        // the last chunk left rather than into eighteen fresh ones. A shorter stack uses the front.
        if (scratch.mGrounds.size() < layers.size())
            scratch.mGrounds.resize(layers.size());

        for (std::size_t index = 0; index < layers.size(); ++index)
            prepare(layers[index], extent, scratch.mGrounds[index]);

        // **`mCoarser` to the same size, because the reduction swaps the two.** Only `mLight` is
        // ever assigned the whole extent, so without this the other leaves the first chunk holding a
        // quarter of it — and where the swaps put that one under the next chunk's sum, it has to
        // grow once more. Reserved, the first chunk pays for both and none after it pays at all.
        const std::size_t texels = std::size_t{ extent } * extent;
        scratch.mCoarser.reserve(texels);
        scratch.mLight.assign(texels, osg::Vec3f());

        // **A layer at a time, and a row of it at a time.** Every tap down the V axis — the mask's
        // row pair, each diffuse level's row pair — belongs to the row rather than to the texel,
        // and a row a layer is absent from is a row of it with nothing to sum. Walking the stack
        // outermost is what lets both be answered once instead of a quarter of a million times.
        //
        // **The stack has to stay in its own order**, because a float sum is the order it was added
        // in: this reaches one texel layer by layer, exactly as a loop over the texels would.
        for (std::size_t index = 0; index < layers.size(); ++index)
        {
            const Ground& ground = scratch.mGrounds[index];

            // A layer whose diffuse would not decode has nothing but black to weigh, and black at
            // any weight leaves the sum where it was.
            if (ground.mFine.isEmpty())
                continue;

            const CompositeLayer& layer = layers[index];

            // A chunk of one ground type is given no mask at all: there is nothing to blend against.
            const bool everywhere = layer.mMaskWidth == 0 || layer.mMaskHeight == 0;
            assert(everywhere || layer.mMask.size() == std::size_t{ layer.mMaskWidth } * layer.mMaskHeight);

            for (std::uint32_t y = 0; y < extent; ++y)
            {
                const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(extent);

                Tap maskDown;
                if (!everywhere)
                {
                    maskDown = clampedTap(v * layer.mMaskTransform.y() + layer.mMaskTransform.w(), layer.mMaskHeight);
                    if (!coveredColumns(layer, maskDown, scratch.mCovered))
                        continue;
                }

                const float atV = v * layer.mDiffuseTransform.y() + layer.mDiffuseTransform.w();
                const Tap fineDown = wrappedTap(atV, ground.mFine.mHeight);
                const bool trilinear = ground.mBetween > 0.0f && !ground.mCoarse.isEmpty();
                const Tap coarseDown = trilinear ? wrappedTap(atV, ground.mCoarse.mHeight) : Tap{};

                osg::Vec3f* const row = scratch.mLight.data() + std::size_t{ y } * extent;

                for (std::uint32_t x = 0; x < extent; ++x)
                {
                    const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(extent);

                    float showing = 1.0f;
                    if (!everywhere)
                    {
                        const Tap maskAcross
                            = clampedTap(u * layer.mMaskTransform.x() + layer.mMaskTransform.z(), layer.mMaskWidth);

                        if (scratch.mCovered[maskAcross.mFirst] == 0 && scratch.mCovered[maskAcross.mSecond] == 0)
                            continue;

                        showing = maskWeight(layer, maskAcross, maskDown);
                        if (showing <= 0.0f)
                            continue;
                    }

                    const float atU = u * layer.mDiffuseTransform.x() + layer.mDiffuseTransform.z();

                    osg::Vec3f colour = sampleAt(ground.mFine, wrappedTap(atU, ground.mFine.mWidth), fineDown);
                    if (trilinear)
                        colour = colour * (1.0f - ground.mBetween)
                            + sampleAt(ground.mCoarse, wrappedTap(atU, ground.mCoarse.mWidth), coarseDown)
                                * ground.mBetween;

                    // **The layer's own texel, at the layer's own tiled coordinates.** The estimate
                    // repeats with the texture and this is the last point at which that tiling is
                    // still known — dividing the finished sum, at the chunk's coordinates, would be
                    // correcting a texture that no longer exists by a map that never described it.
                    if (delight > 0.0f && !layer.mShading.empty())
                        colour /= std::lerp(1.0f, paintedLight(layer.mShading, atU, atV), delight);

                    row[x] += colour * showing;
                }
            }
        }

        buildChain(extent, scratch);
    }

    void TerrainComposite::buildChain(const std::uint32_t extent, CompositeScratch& scratch)
    {
        std::vector<osg::Vec3f>& light = scratch.mLight;

        mTexture.openChain(extent, extent, TextureFormat::Rgba8Srgb);

        const MipPyramid& shape = mTexture.getShape();

        for (std::uint32_t at = 0; at < shape.getLevelCount(); ++at)
        {
            const std::uint32_t side = shape.getLevel(at).mWidth;

            if (at > 0)
            {
                // Box-filtered in light for the reason the blend above is summed in it, and built
                // here rather than left to whatever the file carried: a composite has no file.
                const std::uint32_t finer = side * 2;
                scratch.mCoarser.assign(std::size_t{ side } * side, osg::Vec3f());

                for (std::uint32_t y = 0; y < side; ++y)
                    for (std::uint32_t x = 0; x < side; ++x)
                    {
                        const std::size_t from = std::size_t{ y } * 2 * finer + std::size_t{ x } * 2;
                        scratch.mCoarser[std::size_t{ y } * side + x]
                            = (light[from] + light[from + 1] + light[from + finer] + light[from + finer + 1]) * 0.25f;
                    }

                light.swap(scratch.mCoarser);
            }

            const std::span<std::byte> level = mTexture.level(at);

            for (std::size_t texel = 0; texel < std::size_t{ side } * side; ++texel)
            {
                const osg::Vec3f& colour = light[texel];
                const std::span<std::byte> into = level.subspan(texel * OwnedTexture::sStride);

                into[0] = encodeByte(colour.x());
                into[1] = encodeByte(colour.y());
                into[2] = encodeByte(colour.z());
                into[3] = std::byte{ 255 };
            }
        }
    }

    TextureData TerrainComposite::describe() const
    {
        // **Neutral, and one grid shared by every composite there will ever be.** A texture with no
        // map at all reads whatever the array's stand-in holds, and there is nothing left for a real
        // one to say: the light painted into the ground came off per tile during the bake, which is
        // the only place the tiling was still known.
        static const ShadingMap sNeutral;

        TextureData described = mTexture.describe();
        described.mShading = sNeutral.getValues();

        return described;
    }
}
