#include "scenemasks.hpp"

#include <algorithm>

#include <osg/Image>

#include "error.hpp"
#include "scenedesc.hpp"
#include "texturebuilder.hpp"

namespace Rtx
{
    namespace
    {
        /// A mesh more than one material stands on, which is a mesh no micromap can speak for.
        ///
        /// One below "no material at all", and no more a slot the scene can hand out than that is.
        constexpr Index sManyMaterials = sNoIndex - 1;
    }

    void micromapCandidates(const SceneDesc& scene, std::span<const Index> meshes, std::vector<Index>& materialOfMesh,
        std::vector<MicromapCandidate>& into)
    {
        materialOfMesh.assign(scene.getMeshes().size(), sNoIndex);

        for (const MeshInstance& instance : scene.getInstances())
        {
            if (!instance.isPlaced() || instance.mMesh >= materialOfMesh.size())
                continue;

            Index& held = materialOfMesh[instance.mMesh];
            if (held == sNoIndex)
                held = instance.mMaterial;
            else if (held != instance.mMaterial)
                held = sManyMaterials;
        }

        into.clear();
        into.reserve(meshes.size());

        for (const Index mesh : meshes)
        {
            if (scene.getMeshes()[mesh].mIndexCount == 0)
                continue;

            const Index material = materialOfMesh[mesh];
            if (material == sNoIndex || material == sManyMaterials)
                continue;

            const Material& worn = scene.getMaterials()[material];
            if (!worn.isCutout() || worn.isTranslucent())
                continue;

            into.push_back(MicromapCandidate{ .mMesh = mesh, .mMaterial = material });
        }
    }

    SceneMasks::SceneMasks(const SceneDesc& scene, Resource::ImageManager& images, std::span<const Index> meshes,
        std::span<const TextureData> held)
    {
        std::vector<Index> materialOfMesh;
        std::vector<MicromapCandidate> candidates;
        micromapCandidates(scene, meshes, materialOfMesh, candidates);

        // Which slots the meshes want, each named once. A cell's cutout materials share a handful of
        // images between them — 28 masks over 1580 meshes at Seyda Neen — so this is tens of entries
        // and a linear search over it beats a set that allocates per insert.
        std::vector<Index> wanted;
        for (const MicromapCandidate& candidate : candidates)
        {
            const Index diffuse = scene.getMaterials()[candidate.mMaterial].mDiffuse;
            if (std::find(wanted.begin(), wanted.end(), diffuse) == wanted.end())
                wanted.push_back(diffuse);
        }

        mDescriptions.reserve(wanted.size());
        mImages.reserve(wanted.size());

        // Which slot each opened image belongs to, parallel to `mImages`, because the descriptions
        // cannot be built until every level is counted and the table reserved.
        std::vector<Index> opened;
        opened.reserve(wanted.size());

        for (const Index slot : wanted)
        {
            if (const TextureData* already = textureAt(held, slot))
            {
                mDescriptions.push_back(*already);
                continue;
            }

            // **A free slot and a baked one are both nothing to open.** A slot the scene emptied
            // names no file, and one this renderer painted for itself — a flattened chunk, a sprite
            // light — has bytes only where the queue that made them still holds them. Neither is a
            // cutout any content file wears, and a mesh left unclassified renders correctly.
            if (scene.isTextureFree(slot) || !scene.getBakedTextures()[slot].empty())
                continue;

            if (osg::ref_ptr<const osg::Image> image = openImage(images, scene.getTextures()[slot]))
            {
                mImages.push_back(std::move(image));
                opened.push_back(slot);
            }
        }

        // **Reserved exactly, and that is what makes the spans safe.** Every description points into
        // this one table, so it must not reallocate while they are being taken.
        std::size_t levels = 0;
        for (const osg::ref_ptr<const osg::Image>& image : mImages)
            levels += image->getNumMipmapLevels();
        mLevels.reserve(levels);

        for (std::size_t at = 0; at < mImages.size(); ++at)
        {
            try
            {
                TextureData described = describeImage(*mImages[at], mLevels);
                described.mSlot = opened[at];
                mDescriptions.push_back(described);
                ++mOpened;
            }
            catch (const Error&)
            {
                // A format this renderer does not upload is a mask it cannot read, and the mesh
                // wearing it goes on asking. `SceneTextures` draws the stand-in for the picture; a
                // stand-in here would classify a canopy against mid grey.
            }
        }
    }
}
