#include <components/esm3/cellref.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm3/loadacti.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm3/loadcont.hpp>
#include <components/esm3/loaddoor.hpp>
#include <components/esm3/loadgmst.hpp>
#include <components/esm3/loadland.hpp>
#include <components/esm3/loadstat.hpp>
#include <components/esm3/readerscache.hpp>
#include <components/esmloader/esmdata.hpp>
#include <components/esmloader/load.hpp>
#include <components/files/collections.hpp>
#include <components/files/multidircollection.hpp>
#include <components/toutf8/toutf8.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>

#ifndef OPENMW_DATA_DIR
#error "OPENMW_DATA_DIR is not defined"
#endif

namespace
{
    using namespace testing;
    using namespace EsmLoader;

    struct EsmLoaderTest : Test
    {
        const Files::PathContainer mDataDirs{ { std::filesystem::path{ OPENMW_DATA_DIR } } };
        const Files::Collections mFileCollections{ mDataDirs };
        const std::vector<std::string> mContentFiles{ { "template.omwgame" } };
    };

    TEST_F(EsmLoaderTest, loadEsmDataShouldSupportOmwgame)
    {
        Query query;
        query.mLoadCells = true;
        query.mLoadGameSettings = true;
        query.mLoadLands = true;
        query.mModels = modelRecords<ESM::Activator, ESM::Container, ESM::Door, ESM::Static>();
        ESM::ReadersCache readers;
        ToUTF8::Utf8Encoder* const encoder = nullptr;
        const EsmData esmData = loadEsmData(query, mContentFiles, mFileCollections, readers, encoder);
        EXPECT_EQ(esmData.get<ESM::Activator>().size(), 0);
        EXPECT_EQ(esmData.mCells.size(), 1);
        EXPECT_EQ(esmData.get<ESM::Container>().size(), 0);
        EXPECT_EQ(esmData.get<ESM::Door>().size(), 0);
        EXPECT_EQ(esmData.mGameSettings.size(), 1521);
        EXPECT_EQ(esmData.mLands.size(), 1);
        EXPECT_EQ(esmData.get<ESM::Static>().size(), 2);
    }

    TEST_F(EsmLoaderTest, shouldIgnoreCellsWhenQueryLoadCellsIsFalse)
    {
        Query query;
        query.mLoadCells = false;
        query.mLoadGameSettings = true;
        query.mLoadLands = true;
        query.mModels = modelRecords<ESM::Activator, ESM::Container, ESM::Door, ESM::Static>();
        ESM::ReadersCache readers;
        ToUTF8::Utf8Encoder* const encoder = nullptr;
        const EsmData esmData = loadEsmData(query, mContentFiles, mFileCollections, readers, encoder);
        EXPECT_EQ(esmData.get<ESM::Activator>().size(), 0);
        EXPECT_EQ(esmData.mCells.size(), 0);
        EXPECT_EQ(esmData.get<ESM::Container>().size(), 0);
        EXPECT_EQ(esmData.get<ESM::Door>().size(), 0);
        EXPECT_EQ(esmData.mGameSettings.size(), 1521);
        EXPECT_EQ(esmData.mLands.size(), 1);
        EXPECT_EQ(esmData.get<ESM::Static>().size(), 2);
    }

    TEST_F(EsmLoaderTest, shouldIgnoreCellsGameSettingsWhenQueryLoadGameSettingsIsFalse)
    {
        Query query;
        query.mLoadCells = true;
        query.mLoadGameSettings = false;
        query.mLoadLands = true;
        query.mModels = modelRecords<ESM::Activator, ESM::Container, ESM::Door, ESM::Static>();
        ESM::ReadersCache readers;
        ToUTF8::Utf8Encoder* const encoder = nullptr;
        const EsmData esmData = loadEsmData(query, mContentFiles, mFileCollections, readers, encoder);
        EXPECT_EQ(esmData.get<ESM::Activator>().size(), 0);
        EXPECT_EQ(esmData.mCells.size(), 1);
        EXPECT_EQ(esmData.get<ESM::Container>().size(), 0);
        EXPECT_EQ(esmData.get<ESM::Door>().size(), 0);
        EXPECT_EQ(esmData.mGameSettings.size(), 0);
        EXPECT_EQ(esmData.mLands.size(), 1);
        EXPECT_EQ(esmData.get<ESM::Static>().size(), 2);
    }

    /// A type the mask does not name is skipped even when the file is full of it.
    ///
    /// The gate is per type rather than all-or-nothing because what it lets through decides what the
    /// caller's world is made of: the navmesh tools ask for the four record types that carry
    /// collision, and a navmesh with the lights and the books in it is a different navmesh.
    TEST_F(EsmLoaderTest, shouldIgnoreRecordTypesTheQueryDoesNotName)
    {
        Query query;
        query.mModels = modelRecords<ESM::Activator>();
        ESM::ReadersCache readers;
        ToUTF8::Utf8Encoder* const encoder = nullptr;
        const EsmData esmData = loadEsmData(query, mContentFiles, mFileCollections, readers, encoder);

        // The file holds two of these, and the test above reads them both.
        EXPECT_EQ(esmData.get<ESM::Static>().size(), 0);
        EXPECT_EQ(esmData.mRefIdTypes.size(), 0);
    }

    TEST_F(EsmLoaderTest, shouldIgnoreAllWithDefaultQuery)
    {
        const Query query;
        ESM::ReadersCache readers;
        ToUTF8::Utf8Encoder* const encoder = nullptr;
        const EsmData esmData = loadEsmData(query, mContentFiles, mFileCollections, readers, encoder);
        EXPECT_EQ(esmData.get<ESM::Activator>().size(), 0);
        EXPECT_EQ(esmData.mCells.size(), 0);
        EXPECT_EQ(esmData.get<ESM::Container>().size(), 0);
        EXPECT_EQ(esmData.get<ESM::Door>().size(), 0);
        EXPECT_EQ(esmData.mGameSettings.size(), 0);
        EXPECT_EQ(esmData.mLands.size(), 0);
        EXPECT_EQ(esmData.get<ESM::Static>().size(), 0);
    }

    TEST_F(EsmLoaderTest, loadEsmDataShouldSkipUnsupportedFormats)
    {
        Query query;
        query.mLoadCells = true;
        query.mLoadGameSettings = true;
        query.mLoadLands = true;
        query.mModels = modelRecords<ESM::Activator, ESM::Container, ESM::Door, ESM::Static>();
        const std::vector<std::string> contentFiles{ { "script.omwscripts" } };
        ESM::ReadersCache readers;
        ToUTF8::Utf8Encoder* const encoder = nullptr;
        const EsmData esmData = loadEsmData(query, contentFiles, mFileCollections, readers, encoder);
        EXPECT_EQ(esmData.get<ESM::Activator>().size(), 0);
        EXPECT_EQ(esmData.mCells.size(), 0);
        EXPECT_EQ(esmData.get<ESM::Container>().size(), 0);
        EXPECT_EQ(esmData.get<ESM::Door>().size(), 0);
        EXPECT_EQ(esmData.mGameSettings.size(), 0);
        EXPECT_EQ(esmData.mLands.size(), 0);
        EXPECT_EQ(esmData.get<ESM::Static>().size(), 0);
    }

    /// Two content files written for the purpose: one that places references, one that moves two of
    /// them into other cells.
    ///
    /// **Written and not shipped, because nothing shipped does this.** Morrowind, Tribunal and
    /// Bloodmoon carry not one `MVRF` between them, so content that moves a reference is the only
    /// thing that reaches the merge — and without a file like this the merge would never run at all.
    struct EsmLoaderMovedRefsTest : Test
    {
        static constexpr std::uint32_t sStayingRefIndex = 1;
        static constexpr std::uint32_t sWildernessRefIndex = 2;

        const std::filesystem::path mDir = writeContent();
        const Files::PathContainer mDataDirs{ { mDir } };
        const Files::Collections mFileCollections{ mDataDirs };
        const std::vector<std::string> mContentFiles{ "moved-base.omwgame", "moved-plugin.omwaddon" };

        ~EsmLoaderMovedRefsTest() override { std::filesystem::remove_all(mDir); }

        static ESM::Cell exterior(int x, int y, std::string_view name)
        {
            ESM::Cell cell{};
            cell.mName = std::string(name);
            cell.mData.mFlags = ESM::Cell::HasWater;
            cell.mData.mX = x;
            cell.mData.mY = y;
            cell.updateId();
            return cell;
        }

        /// One reference as a content file carries it.
        ///
        /// **`plugin` is which file placed it**, zero for this file's own and counted from one
        /// against this file's masters. It is the high byte of the packed `FRMR`, and reading it
        /// back out is the whole of what `adjustRefNum` does.
        static ESM::CellRef reference(std::uint32_t index, std::int32_t plugin, float x)
        {
            ESM::CellRef ref;
            ref.blank();
            ref.mRefNum.mIndex = index;
            ref.mRefNum.mContentFile = plugin;
            ref.mRefID = ESM::RefId::stringRefId("test_stat");
            ref.mPos = ESM::Position{};
            ref.mPos.pos[0] = x;
            return ref;
        }

        /// An `MVRF` naming where a reference went, the `CNDT` giving the cell, and the reference
        /// itself at the position it now stands.
        static void writeMoved(ESM::ESMWriter& writer, std::uint32_t index, int x, int y, float at)
        {
            constexpr std::uint32_t fromFirstMaster = 1u << 24;

            writer.writeHNT("MVRF", index | fromFirstMaster);
            const std::int32_t target[2] = { x, y };
            writer.writeHNT("CNDT", target);
            reference(index, 1, at).save(writer);
        }

        static std::filesystem::path writeContent()
        {
            const std::filesystem::path dir = std::filesystem::temp_directory_path() / "openmw-esmloader-moved-refs";
            std::filesystem::remove_all(dir);
            std::filesystem::create_directories(dir);

            {
                std::ofstream out(dir / "moved-base.omwgame", std::ios::binary);
                ESM::ESMWriter writer;
                writer.setFormatVersion(ESM::CurrentContentFormatVersion);
                writer.save(out);

                writer.startRecord(ESM::REC_CELL);
                exterior(0, 0, "source").save(writer, false);
                reference(sStayingRefIndex, 0, 0.0f).save(writer);
                reference(sWildernessRefIndex, 0, 0.0f).save(writer);
                writer.endRecord(ESM::REC_CELL);

                writer.startRecord(ESM::REC_CELL);
                exterior(1, 0, "authored").save(writer, false);
                writer.endRecord(ESM::REC_CELL);

                writer.close();
            }

            {
                std::ofstream out(dir / "moved-plugin.omwaddon", std::ios::binary);
                ESM::ESMWriter writer;
                writer.setFormatVersion(ESM::CurrentContentFormatVersion);
                writer.addMaster("moved-base.omwgame", 0);
                writer.save(out);

                writer.startRecord(ESM::REC_CELL);
                exterior(0, 0, "source").save(writer, false);

                writeMoved(writer, sStayingRefIndex, 1, 0, 1000.0f);
                writeMoved(writer, sWildernessRefIndex, 5, 5, 2000.0f);

                writer.endRecord(ESM::REC_CELL);
                writer.close();
            }

            return dir;
        }

        static const ESM::Cell* findExterior(const EsmData& data, int x, int y)
        {
            for (const ESM::Cell& cell : data.mCells)
                if (cell.isExterior() && cell.getGridX() == x && cell.getGridY() == y)
                    return &cell;

            return nullptr;
        }
    };

    /// A reference one content file moves out of a cell leaves it and arrives in the other.
    ///
    /// **Nothing in the shipped game exercises this**: Morrowind, Tribunal and Bloodmoon carry not
    /// one `MVRF` between them, so the merge is reached only by content that moves references —
    /// which is why it is reached here, by content written for the purpose.
    ///
    /// **Both halves, because a moved reference is two facts in two cells.** The cell that had it
    /// carries an `MVRF` naming a destination and must stop placing it where it used to be; the
    /// destination's own reference block never mentions it and must be handed the reference itself.
    /// Getting one and not the other is a world with the thing in two places, or in none.
    ///
    /// **Two moves, because the destination need not exist.** One lands in a cell the base file
    /// authored; the other lands in wilderness nothing has written a `CELL` for, which the loader
    /// has to invent the way the game's `searchOrCreate` does.
    TEST_F(EsmLoaderMovedRefsTest, aReferenceMovedByALaterFileLeavesItsCellAndArrivesInAnother)
    {
        Query query;
        query.mLoadCells = true;

        ESM::ReadersCache readers;
        const EsmData data = loadEsmData(query, mContentFiles, mFileCollections, readers, /*encoder=*/nullptr);

        const ESM::Cell* source = findExterior(data, 0, 0);
        ASSERT_NE(source, nullptr);

        ASSERT_EQ(source->mMovedRefs.size(), 2);
        const auto moved = source->mMovedRefs.begin();
        EXPECT_EQ(moved->mRefNum.mIndex, sStayingRefIndex);
        EXPECT_EQ(moved->mRefNum.mContentFile, 0);
        EXPECT_EQ(moved->mTarget[0], 1);
        EXPECT_EQ(moved->mTarget[1], 0);
        EXPECT_EQ(std::next(moved)->mTarget[0], 5);
        EXPECT_EQ(std::next(moved)->mTarget[1], 5);

        // The destination the base file authored keeps everything it already had and gains the one
        // reference that moved into it.
        const ESM::Cell* authored = findExterior(data, 1, 0);
        ASSERT_NE(authored, nullptr);
        EXPECT_EQ(authored->mName, "authored");
        ASSERT_EQ(authored->mLeasedRefs.size(), 1);
        EXPECT_EQ(authored->mLeasedRefs.front().first.mRefNum.mIndex, sStayingRefIndex);
        EXPECT_EQ(authored->mLeasedRefs.front().first.mRefNum.mContentFile, 0);

        // **Where it now stands and not where it stood**, which is the whole point of leasing the
        // reference rather than only recording that it left.
        EXPECT_EQ(authored->mLeasedRefs.front().first.mPos.pos[0], 1000.0f);

        // A cell nothing authored, invented so the reference has somewhere to land.
        const ESM::Cell* invented = findExterior(data, 5, 5);
        ASSERT_NE(invented, nullptr) << "the wilderness the reference moved into was never made";
        ASSERT_EQ(invented->mLeasedRefs.size(), 1);
        EXPECT_EQ(invented->mLeasedRefs.front().first.mRefNum.mIndex, sWildernessRefIndex);
    }
}
