#pragma once

#include <cstdint>
#include <span>

#include <osg/BoundingBox>

#include "deformertable.hpp"
#include "index.hpp"
#include "light.hpp"
#include "materialtable.hpp"
#include "meshtable.hpp"
#include "placementtable.hpp"
#include "shaders/skinning.h"
#include "sprite.hpp"
#include "texturetable.hpp"

namespace Rtx
{
    /// What a reader of a scene is handed: the five tables, and the three lists that belong to no
    /// table.
    ///
    /// **A view and not the scene.** Handing the tables over says once what a forwarder per field
    /// would say many times, and it says something forwarders could not: a reader cannot move an
    /// instance or free a texture, because it never had the scene.
    ///
    /// Borrowed, and valid for as long as the scene it came from is. Cheap to make and meant to be
    /// made per call rather than held.
    struct SceneTables
    {
        const MeshTable& mMeshes;
        const DeformerTable& mDeformers;
        const PlacementTable& mPlacements;
        const MaterialTable& mMaterials;
        const TextureTable& mTextures;

        std::span<const Light> mLights;
        std::span<const Sprite> mSprites;
        std::span<const SpriteEmitter> mEmitters;

        /// What a backend compares against to know whether the geometry or the textures it built
        /// from are still the ones the scene holds.
        std::uint64_t getStructureRevision() const { return mMeshes.getRevision() + mTextures.getRevision(); }

        /// The pose a deforming mesh was last given, and the weights a morphed one carries. Both
        /// cross two tables: the mesh row says where its run sits, and the deformers hold it.
        std::span<const Shaders::GpuBone> getMeshBones(Index mesh) const;
        std::span<const float> getMeshWeights(Index mesh) const;

        /// Every placement's own box, in the world.
        osg::BoundingBoxf getBounds() const;

        /// The same, clipped to `region` and with the water left out.
        ///
        /// **The sea is one sheet a hundred and fifty cells across**, so a caller asking how far the
        /// ground reaches would clear any threshold at every coastline.
        osg::BoundingBoxf getContentBoundsWithin(const osg::BoundingBoxf& region) const;

    private:
        template <class Visit>
        void forEachPlacement(Visit&& visit) const;
    };
}
