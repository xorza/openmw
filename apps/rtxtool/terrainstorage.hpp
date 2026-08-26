#pragma once

#include <cstdint>
#include <map>
#include <utility>

#include <components/esmterrain/storage.hpp>

namespace EsmLoader
{
    struct EsmData;
}

namespace RtxTool
{
    /// Connects the content files this tool loaded to OpenMW's terrain builder.
    ///
    /// The game's equivalent is `MWRender::TerrainStorage`, which sits on the world's record store
    /// and a resource-managed land cache. This has neither, so it indexes `EsmLoader::EsmData`'s
    /// land records once and caches what it decodes.
    class TerrainStorage : public ESMTerrain::Storage
    {
    public:
        TerrainStorage(const VFS::Manager& vfs, const EsmLoader::EsmData& esmData);

        osg::ref_ptr<const ESMTerrain::LandObject> getLand(ESM::ExteriorCellLocation cellLocation) override;

        /// What the plugin that wrote the cell called index `index`, or null.
        ///
        /// The caller falls back to `_land_default.dds` and warns when this finds nothing, which is
        /// the right answer: the shipped data does dangle, and a layer quietly taking the default is
        /// worth a line.
        const VFS::Path::Normalized* getLandTexture(std::uint16_t index, int plugin) override;

        /// Null: this tool reads Morrowind, and these are Bethesda's later formats.
        const ESM4::LandTexture* getEsm4LandTexture(ESM::RefId ltexId) const override;
        const ESM4::TextureSet* getEsm4TextureSet(ESM::RefId txstId) const override;

        bool hasData(ESM::ExteriorCellLocation cellLocation) override;

        void getBounds(float& minX, float& maxX, float& minY, float& maxY, ESM::RefId worldspace) override;

    private:
        using CellKey = std::pair<int, int>;

        const ESM::Land* findLand(ESM::ExteriorCellLocation cellLocation) const;

        /// Pointers into the caller's `EsmData`, which therefore has to outlive this.
        std::map<CellKey, const ESM::Land*> mLands;

        /// Kept whole for the land textures, which are already one binary search and want no index.
        const EsmLoader::EsmData& mEsmData;

        // Decoding a land record is the expensive half, and a chunk asks for its neighbours as well
        // as itself, so the same record is wanted several times over.
        std::map<CellKey, osg::ref_ptr<const ESMTerrain::LandObject>> mDecoded;
    };
}
