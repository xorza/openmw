#include "compositequeue.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <optional>

#include <components/resource/imagemanager.hpp>

#include "error.hpp"
#include "shadingmap.hpp"
#include "texturebuilder.hpp"

namespace Rtx
{
    namespace
    {
        /// The key a chunk's composite is found under.
        ///
        /// **The material's own slot, because one material is one chunk.** The extractor keys a
        /// terrain material on the state set it came from, so the two are already one to one; a
        /// material that is retired takes its composite's slot with it, and one that takes the slot
        /// over is a different chunk asking for a different bake under the same name — which is
        /// exactly right, because it wants the slot overwritten.
        void nameComposite(std::string& key, Index material)
        {
            std::array<char, 16> digits{};
            const auto written = std::to_chars(digits.data(), digits.data() + digits.size(), material, 16);

            key.assign("chunk/");
            key.append(digits.data(), written.ptr);
        }
    }

    void CompositeQueue::gather(const SceneDesc& scene, Resource::ImageManager& images)
    {
        // **A cleared scene renumbers everything, so nothing here still refers to anything.**
        // `SceneDesc::clear` empties the material table and starts the indices again; a chunk that
        // was waiting is waiting on a material that no longer exists, and the index it kept would
        // read past the end of a table that has just been emptied.
        if (mReset != scene.getResetRevision())
        {
            mReset = scene.getResetRevision();
            mWaiting.clear();
            mFinished.clear();
        }

        if (mScanned == scene.getShadingRevision())
            return;

        mScanned = scene.getShadingRevision();

        // Held only for as long as this runs: the layers below span it, and the composites they
        // build copy what they are handed.
        mPainted.clear();

        const std::span<const Material> materials = scene.getMaterials();
        for (Index at = 0; at < materials.size(); ++at)
        {
            const Material& material = materials[at];
            if (material.mKind != MaterialKind::Terrain || !material.mFlatten || material.mDiffuse != sNoIndex)
                continue;

            const auto waiting = std::find_if(
                mWaiting.begin(), mWaiting.end(), [&](const Waiting& one) { return one.mMaterial == at; });
            if (waiting != mWaiting.end() && waiting->mLayerOffset == material.mLayerOffset
                && waiting->mLayerCount == material.mLayerCount)
                continue;

            // A slot taken over by another chunk while its predecessor was baking: what is half done
            // is a picture of ground that has gone, so it starts again rather than finishing.
            if (waiting != mWaiting.end())
                mWaiting.erase(waiting);

            const std::span<const MaterialLayer> layers
                = scene.getLayers().subspan(material.mLayerOffset, material.mLayerCount);

            mLevels.clear();
            mStack.clear();
            mStack.reserve(layers.size());

            // **Reserved before anything points into it.** Every description below spans `mLevels`,
            // so a reallocation part way through would leave the bake reading where the earlier
            // layers used to be.
            mSources.clear();
            mSources.reserve(layers.size());

            std::size_t count = 0;
            for (const MaterialLayer& layer : layers)
            {
                mSources.push_back(openImage(images, scene.getTextures()[layer.mDiffuse]));
                count += mSources.back() != nullptr ? mSources.back()->getNumMipmapLevels() : 0;
            }
            mLevels.reserve(count);

            for (std::size_t index = 0; index < layers.size(); ++index)
            {
                if (mSources[index] == nullptr)
                    continue;

                std::optional<TextureData> described;
                try
                {
                    described = describeImage(*mSources[index], mLevels);
                }
                catch (const Error&)
                {
                    continue;
                }

                const auto estimate = mPainted.try_emplace(layers[index].mDiffuse, *described).first;

                mStack.push_back(CompositeLayer{
                    .mDiffuse = *described,
                    .mShading = estimate->second.getValues(),
                    .mDiffuseTransform = layers[index].mDiffuseTransform,
                    .mMask = scene.getMasks().subspan(
                        layers[index].mMaskOffset, std::size_t{ layers[index].mMaskWidth } * layers[index].mMaskHeight),
                    .mMaskWidth = layers[index].mMaskWidth,
                    .mMaskHeight = layers[index].mMaskHeight,
                    .mMaskTransform = layers[index].mMaskTransform,
                });
            }

            // Every layer unreadable is a chunk with nothing to flatten. It keeps its stack, which
            // is what it was already shading from, and asks again no more than the walk does.
            if (mStack.empty())
                continue;

            mWaiting.push_back(Waiting{
                .mMaterial = at,
                .mLayerOffset = material.mLayerOffset,
                .mLayerCount = material.mLayerCount,
                .mComposite = TerrainComposite(mStack, sCompositeExtent, sCompositeDelight),
            });
        }
    }

    std::size_t CompositeQueue::drain(SceneDesc& scene, const std::uint32_t rows)
    {
        std::size_t finished = 0;
        std::uint32_t left = rows;

        while (left > 0 && !mWaiting.empty())
        {
            Waiting& one = mWaiting.front();

            const std::uint32_t before = one.mComposite.getBakedRows();
            const bool done = one.mComposite.bake(left);
            left -= one.mComposite.getBakedRows() - before;

            if (!done)
                break;

            const std::span<const Material> materials = scene.getMaterials();
            assert(one.mMaterial < materials.size() && "a composite waiting on a material the scene has forgotten");

            const Material& material = materials[one.mMaterial];

            // **What it baked has to still be what stands there.** A chunk can leave the world in
            // the frames a composite takes, and the slot it stood in can be taken over by another;
            // handing this to whatever holds the slot now would put one hillside's ground on
            // another's.
            const bool wanted = material.mKind == MaterialKind::Terrain && material.mFlatten
                && material.mDiffuse == sNoIndex && material.mLayerOffset == one.mLayerOffset
                && material.mLayerCount == one.mLayerCount;

            if (wanted)
            {
                nameComposite(mKey, one.mMaterial);

                Material given = material;
                given.mDiffuse = scene.addBakedTexture(mKey);
                scene.setMaterial(one.mMaterial, given);

                mFinished.insert_or_assign(given.mDiffuse, std::move(one.mComposite));
                ++finished;
            }

            mWaiting.erase(mWaiting.begin());
        }

        return finished;
    }

    const TerrainComposite* CompositeQueue::find(const Index slot) const
    {
        const auto found = mFinished.find(slot);
        return found == mFinished.end() ? nullptr : &found->second;
    }
}
