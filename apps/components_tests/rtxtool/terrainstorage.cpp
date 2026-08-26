#include <gtest/gtest.h>

#include <components/esm/exteriorcelllocation.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm3/loadland.hpp>
#include <components/esmloader/esmdata.hpp>

// `EsmData`'s move constructor is defaulted in the class, so moving one out of a helper needs every
// record type it holds to be complete, not just the ones this file names.
#include <components/esm3/loadacti.hpp>
#include <components/esm3/loadcont.hpp>
#include <components/esm3/loaddoor.hpp>
#include <components/esm3/loadgmst.hpp>
#include <components/esm3/loadstat.hpp>
#include <components/vfs/manager.hpp>

#include <apps/rtxtool/terrainstorage.hpp>

namespace RtxTool
{
    namespace
    {
        EsmLoader::EsmData makeLands(std::initializer_list<std::pair<int, int>> cells)
        {
            EsmLoader::EsmData data;
            for (const auto& [x, y] : cells)
            {
                ESM::Land land;
                land.mX = x;
                land.mY = y;
                data.mLands.push_back(std::move(land));
            }
            return data;
        }

        ESM::ExteriorCellLocation at(int x, int y)
        {
            return ESM::ExteriorCellLocation(x, y, ESM::Cell::sDefaultWorldspaceId);
        }

        /// The chunk builder asks about the cells around the one it is building, so "there is no land
        /// here" is a normal answer and has to be the right one. Getting it wrong either drops a
        /// chunk that should exist or reads a neighbour's heights into an edge that has none.
        TEST(RtxTerrainStorageTest, landIsFoundOnlyWhereThereIsSome)
        {
            const VFS::Manager vfs;
            const EsmLoader::EsmData data = makeLands({ { 0, 0 }, { 1, 0 }, { -2, -9 } });
            TerrainStorage storage(vfs, data);

            EXPECT_TRUE(storage.hasData(at(0, 0)));
            EXPECT_TRUE(storage.hasData(at(1, 0)));
            EXPECT_TRUE(storage.hasData(at(-2, -9)));

            EXPECT_FALSE(storage.hasData(at(0, 1)));
            EXPECT_FALSE(storage.hasData(at(-1, 0)));
            EXPECT_FALSE(storage.hasData(at(-2, -8)));
        }

        /// In cell units, and inclusive of the far edge of the last cell — a cell at x = 1 occupies
        /// the span from 1 to 2.
        TEST(RtxTerrainStorageTest, theBoundsSpanEveryCellWithLand)
        {
            const VFS::Manager vfs;
            const EsmLoader::EsmData data = makeLands({ { 0, 0 }, { 1, 0 }, { -2, -9 } });
            TerrainStorage storage(vfs, data);

            float minX = 0.0f;
            float maxX = 0.0f;
            float minY = 0.0f;
            float maxY = 0.0f;
            storage.getBounds(minX, maxX, minY, maxY, ESM::Cell::sDefaultWorldspaceId);

            EXPECT_FLOAT_EQ(minX, -2.0f);
            EXPECT_FLOAT_EQ(maxX, 2.0f);
            EXPECT_FLOAT_EQ(minY, -9.0f);
            EXPECT_FLOAT_EQ(maxY, 1.0f);
        }

        TEST(RtxTerrainStorageTest, aWorldWithNoLandHasNoBoundsRatherThanInfiniteOnes)
        {
            const VFS::Manager vfs;
            const EsmLoader::EsmData data;
            TerrainStorage storage(vfs, data);

            float minX = 1.0f;
            float maxX = 1.0f;
            float minY = 1.0f;
            float maxY = 1.0f;
            storage.getBounds(minX, maxX, minY, maxY, ESM::Cell::sDefaultWorldspaceId);

            EXPECT_FLOAT_EQ(minX, 0.0f);
            EXPECT_FLOAT_EQ(maxX, 0.0f);
            EXPECT_FLOAT_EQ(minY, 0.0f);
            EXPECT_FLOAT_EQ(maxY, 0.0f);
        }
    }
}
