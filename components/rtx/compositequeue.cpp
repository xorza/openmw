#include "compositequeue.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <span>
#include <string_view>

#include "shaders/scene.h"

namespace Rtx
{
    namespace
    {
        /// The key a chunk's composite is found under — `chunk/` — or its gloss — `gloss/`: the
        /// material's own slot, because one material is one chunk, and one that takes the slot over
        /// is a different chunk that wants the slot overwritten.
        void nameComposite(std::string& key, std::string_view kind, Index material)
        {
            std::array<char, 16> digits{};
            const auto written = std::to_chars(digits.data(), digits.data() + digits.size(), material, 16);

            key.assign(kind);
            key.append(digits.data(), written.ptr);
        }

        /// Whether any layer of `layers` reflects, which is whether a gloss says anything.
        bool reflects(const SceneDesc& scene, const Run& layers)
        {
            return std::ranges::any_of(layers.in(scene.materials().getLayers()),
                [](const MaterialLayer& layer) { return (layer.mFlags & Shaders::LAYER_AUTHORED) != 0u; });
        }

        /// Whether `material` still wants the ground it asked for as `asked`: the same kind, still
        /// asking, not yet given, and its layers where they were. The run and not the layers in
        /// it: the bake reads the row as it stands when the slot is given, so a run handed out
        /// again to another chunk flattens that chunk's ground and costs it only its place in
        /// line.
        bool stillWants(const SceneDesc& scene, const Index material, const Run& layers)
        {
            const std::span<const Material> materials = scene.materials().getRows();
            if (material >= materials.size())
                return false;

            const Material& row = materials[material];
            return row.mKind == MaterialKind::Terrain && row.mFlatten && row.mDiffuse == sNoIndex
                && row.mLayers == layers;
        }
    }

    std::size_t CompositeQueue::advance(SceneDesc& scene)
    {
        gather(scene);
        return take(scene, sCompositesPerFrame);
    }

    void CompositeQueue::gather(const SceneDesc& scene)
    {
        const std::span<const Material> materials = scene.materials().getRows();
        for (const Index at : scene.materials().getWritten())
        {
            const Material& material = materials[at];
            if (material.mKind != MaterialKind::Terrain || !material.mFlatten || material.mDiffuse != sNoIndex)
                continue;

            const Asked wanted{ .mMaterial = at, .mLayers = material.mLayers };

            const auto waiting
                = std::find_if(mWaiting.begin(), mWaiting.end(), [&](const Asked& one) { return one.mMaterial == at; });
            if (waiting != mWaiting.end() && *waiting == wanted)
                continue;

            // A slot taken over by another chunk while its predecessor waited: what was asked is
            // ground that has gone, and the new chunk goes to the back of the schedule.
            if (waiting != mWaiting.end())
                mWaiting.erase(waiting);

            mWaiting.push_back(wanted);
        }
    }

    std::size_t CompositeQueue::take(SceneDesc& scene, const std::size_t limit)
    {
        std::size_t finished = 0;
        while (finished < limit && !mWaiting.empty())
        {
            const Asked asked = mWaiting.front();
            mWaiting.pop_front();

            // What it asked for has to still be what stands there, or one hillside's ground lands
            // on another's.
            if (!stillWants(scene, asked.mMaterial, asked.mLayers))
                continue;

            nameComposite(mKey, "chunk/", asked.mMaterial);

            // A table with no room left keeps the chunk on its stack, which the shader sums at the
            // hit as it does for every chunk still waiting.
            const Index slot = scene.textures().addBaked(mKey);
            if (slot == sNoIndex)
                continue;

            Material given = scene.materials().getRows()[asked.mMaterial];
            given.mDiffuse = slot;
            mFinished.push_back(Given{ .mSlot = slot, .mBaked = { .mMaterial = asked.mMaterial, .mGloss = false } });

            // A table with room for the albedo and not the gloss flattens the chunk with no lobe,
            // which is what it was before it could have one.
            if (reflects(scene, given.mLayers))
            {
                nameComposite(mKey, "gloss/", asked.mMaterial);
                given.mSpecular = scene.textures().addBaked(mKey);
                if (given.mSpecular != sNoIndex)
                    mFinished.push_back(
                        Given{ .mSlot = given.mSpecular, .mBaked = { .mMaterial = asked.mMaterial, .mGloss = true } });
            }

            scene.setMaterial(asked.mMaterial, given);
            ++finished;
        }

        return finished;
    }

    CompositeQueue::Baked CompositeQueue::find(const Index slot) const
    {
        for (const Given& finished : mFinished)
            if (finished.mSlot == slot)
                return finished.mBaked;

        return Baked{};
    }
}
