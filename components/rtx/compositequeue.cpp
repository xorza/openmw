#include "compositequeue.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <span>

namespace Rtx
{
    namespace
    {
        /// The key a chunk's composite is found under: the material's own slot, because one
        /// material is one chunk, and one that takes the slot over is a different chunk that wants
        /// the slot overwritten.
        void nameComposite(std::string& key, Index material)
        {
            std::array<char, 16> digits{};
            const auto written = std::to_chars(digits.data(), digits.data() + digits.size(), material, 16);

            key.assign("chunk/");
            key.append(digits.data(), written.ptr);
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

            nameComposite(mKey, asked.mMaterial);

            Material given = scene.materials().getRows()[asked.mMaterial];
            given.mDiffuse = scene.textures().addBaked(mKey);
            scene.setMaterial(asked.mMaterial, given);

            mFinished.push_back(Given{ .mSlot = given.mDiffuse, .mMaterial = asked.mMaterial });
            ++finished;
        }

        return finished;
    }

    Index CompositeQueue::find(const Index slot) const
    {
        for (const Given& finished : mFinished)
            if (finished.mSlot == slot)
                return finished.mMaterial;

        return sNoIndex;
    }
}
