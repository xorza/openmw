#include "terrainstorage.hpp"

#include <limits>

#include <components/esm/util.hpp>
#include <components/esm3/loadland.hpp>
#include <components/esmloader/esmdata.hpp>

namespace RtxTool
{
    namespace
    {
        /// Everything a chunk needs off a land record: heights, normals, vertex colours and the
        /// texture indices that decide which layers it blends.
        constexpr int sLoadFlags
            = ESM::Land::DATA_VCLR | ESM::Land::DATA_VHGT | ESM::Land::DATA_VNML | ESM::Land::DATA_VTEX;
    }

    TerrainStorage::TerrainStorage(const VFS::Manager& vfs, const EsmLoader::EsmData& esmData)
        : ESMTerrain::Storage(&vfs)
        , mEsmData(esmData)
    {
        for (const ESM::Land& land : esmData.mLands)
            mLands[CellKey(land.mX, land.mY)] = &land;
    }

    const ESM::Land* TerrainStorage::findLand(ESM::ExteriorCellLocation cellLocation) const
    {
        if (ESM::isEsm4Ext(cellLocation.mWorldspace))
            return nullptr;

        const auto found = mLands.find(CellKey(cellLocation.mX, cellLocation.mY));
        return found == mLands.end() ? nullptr : found->second;
    }

    osg::ref_ptr<const ESMTerrain::LandObject> TerrainStorage::getLand(ESM::ExteriorCellLocation cellLocation)
    {
        const CellKey key(cellLocation.mX, cellLocation.mY);

        const auto cached = mDecoded.find(key);
        if (cached != mDecoded.end())
            return cached->second;

        const ESM::Land* land = findLand(cellLocation);
        osg::ref_ptr<const ESMTerrain::LandObject> decoded;
        if (land != nullptr)
            decoded = new ESMTerrain::LandObject(*land, sLoadFlags);

        // Cached either way: a cell with no land is asked about as often as one with land, because
        // every chunk asks about the eight cells around it.
        mDecoded.emplace(key, decoded);
        return decoded;
    }

    const VFS::Path::Normalized* TerrainStorage::getLandTexture(std::uint16_t index, int plugin)
    {
        return EsmLoader::getLandTexture(mEsmData, index, plugin);
    }

    const ESM4::LandTexture* TerrainStorage::getEsm4LandTexture(ESM::RefId /*ltexId*/) const
    {
        return nullptr;
    }

    const ESM4::TextureSet* TerrainStorage::getEsm4TextureSet(ESM::RefId /*txstId*/) const
    {
        return nullptr;
    }

    bool TerrainStorage::hasData(ESM::ExteriorCellLocation cellLocation)
    {
        return findLand(cellLocation) != nullptr;
    }

    void TerrainStorage::getBounds(float& minX, float& maxX, float& minY, float& maxY, ESM::RefId worldspace)
    {
        if (ESM::isEsm4Ext(worldspace) || mLands.empty())
        {
            minX = maxX = minY = maxY = 0.0f;
            return;
        }

        minX = std::numeric_limits<float>::max();
        minY = std::numeric_limits<float>::max();
        maxX = std::numeric_limits<float>::lowest();
        maxY = std::numeric_limits<float>::lowest();

        for (const auto& [key, unused] : mLands)
        {
            minX = std::min(minX, static_cast<float>(key.first));
            minY = std::min(minY, static_cast<float>(key.second));
            maxX = std::max(maxX, static_cast<float>(key.first + 1));
            maxY = std::max(maxY, static_cast<float>(key.second + 1));
        }
    }
}
