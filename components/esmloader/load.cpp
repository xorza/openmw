#include "load.hpp"
#include "esmdata.hpp"
#include "lessbyid.hpp"
#include "record.hpp"

#include <components/debug/debuglog.hpp>
#include <components/esm/defs.hpp>
#include <components/esm/typetraits.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm3/loadgmst.hpp>
#include <components/esm3/loadland.hpp>
#include <components/esm3/loadltex.hpp>
#include <components/esm3/loadregn.hpp>
#include <components/esm3/readerscache.hpp>
#include <components/files/collections.hpp>
#include <components/files/conversion.hpp>
#include <components/files/multidircollection.hpp>
#include <components/loadinglistener/loadinglistener.hpp>
#include <components/misc/pathhelpers.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/strings/lower.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace EsmLoader
{
    namespace
    {
        struct GetKey
        {
            template <class T>
            decltype(auto) operator()(const T& v) const
            {
                return (v.mId);
            }

            const ESM::RefId& operator()(const ESM::Cell& v) const { return v.mId; }

            std::pair<int, int> operator()(const ESM::Land& v) const { return std::pair(v.mX, v.mY); }

            template <class T>
            decltype(auto) operator()(const Record<T>& v) const
            {
                return (*this)(v.mValue);
            }
        };

        struct CellRecords
        {
            Records<ESM::Cell> mValues;
            std::map<std::string, std::size_t> mByName;
            std::map<std::pair<int, int>, std::size_t> mByPosition;
        };

        template <class T>
        concept NotHasId = !ESM::HasId<T>;

        template <ESM::HasId T>
        void loadRecord(ESM::ESMReader& reader, Records<T>& records)
        {
            T record;
            bool deleted = false;
            record.load(reader, deleted);
            if (Misc::ResourceHelpers::isHiddenMarker(record.mId))
                return;
            records.emplace_back(deleted, std::move(record));
        }

        template <NotHasId T>
        void loadRecord(ESM::ESMReader& reader, Records<T>& records)
        {
            T record;
            bool deleted = false;
            record.load(reader, deleted);
            records.emplace_back(deleted, std::move(record));
        }

        /// Where the record for the exterior cell at these coordinates is, adding an empty one if
        /// the content files never authored a `CELL` for it.
        ///
        /// **A moved reference can land in wilderness nobody wrote a record for**, and it still has
        /// to land there. `MWWorld::Store<ESM::Cell>::searchOrCreate` invents the same cell, down to
        /// the water it gives it.
        std::size_t exteriorAt(CellRecords& records, int x, int y)
        {
            const std::pair<int, int> position(x, y);
            const auto it = records.mByPosition.find(position);
            if (it != records.mByPosition.end())
                return it->second;

            ESM::Cell created;
            created.mData.mX = x;
            created.mData.mY = y;
            created.mData.mFlags = ESM::Cell::HasWater;
            created.updateId();

            const std::size_t at = records.mValues.size();
            records.mByPosition.emplace_hint(it, position, at);
            records.mValues.emplace_back(false, std::move(created));
            return at;
        }

        /// Records the references a content file moves out of this cell and into another.
        ///
        /// **Where a moved reference lives is split across two cells, and neither says so alone.**
        /// The cell that used to hold it carries an `MVRF` naming the destination; the destination's
        /// own reference block never mentions it. So the source has to record that the reference
        /// left, or every reader goes on placing it where it used to be, and the destination has to
        /// be handed the reference itself, or nothing places it at all.
        ///
        /// **Merged across the files rather than read out of one**, because only the file that did
        /// the moving carries the `MVRF`: a reader walking an earlier file's block finds the
        /// reference exactly where it was first placed and nothing in that block says otherwise.
        ///
        /// This is `MWWorld::Store<ESM::Cell>::handleMovedCellRefs` against this loader's cell table,
        /// and it must stay that: a world assembled here and one assembled by the game disagreeing
        /// about where a reference stands is the whole class of bug both are meant to rule out.
        ///
        /// **By index and not by reference**, because the destination may be a cell nothing has
        /// authored: adding it can reallocate the very vector the source lives in.
        ///
        /// The reader must be positioned where `loadCell` left it, and is put back.
        void handleMovedRefs(ESM::ESMReader& reader, std::size_t source, CellRecords& records)
        {
            const ESM::ESM_Context context = reader.getContext();

            ESM::CellRef ref;
            ESM::MovedCellRef movedRef;
            bool deleted = false;
            bool moved = false;

            while (
                ESM::Cell::getNextRef(reader, ref, deleted, movedRef, moved, ESM::Cell::GetNextRefMode::LoadOnlyMoved))
            {
                if (!moved)
                    continue;

                const std::size_t target = exteriorAt(records, movedRef.mTarget[0], movedRef.mTarget[1]);
                records.mValues[source].mValue.mMovedRefs.push_back(movedRef);

                ESM::CellRefTracker& leased = records.mValues[target].mValue.mLeasedRefs;

                // **Written over rather than appended to**, because a later content file may move
                // the same reference again and the destination must end up holding one of it.
                const auto held = std::find_if(leased.begin(), leased.end(), ESM::CellRefTrackerPredicate(ref.mRefNum));
                if (held == leased.end())
                    leased.emplace_back(std::move(ref), deleted);
                else
                    *held = std::make_pair(std::move(ref), deleted);

                movedRef.mRefNum.mIndex = 0;
            }

            reader.restoreContext(context);
        }

        void loadRecord(ESM::ESMReader& reader, CellRecords& records)
        {
            ESM::Cell record;
            bool deleted = false;
            record.loadNameAndData(reader, deleted);

            if ((record.mData.mFlags & ESM::Cell::Interior) != 0)
            {
                // **No moved references, because an interior is never the destination of one** — the
                // content file format has nowhere to write anything but a pair of exterior
                // coordinates. The game reads interiors the same way and for the same reason.
                const auto it = records.mByName.find(record.mName);
                if (it == records.mByName.end())
                {
                    record.loadCell(reader, true);
                    records.mByName.emplace_hint(it, record.mName, records.mValues.size());
                    records.mValues.emplace_back(deleted, std::move(record));
                }
                else
                {
                    Record<ESM::Cell>& old = records.mValues[it->second];
                    old.mValue.mData = record.mData;
                    old.mValue.loadCell(reader, true);
                }

                return;
            }

            const std::pair<int, int> position(record.mData.mX, record.mData.mY);
            const auto it = records.mByPosition.find(position);
            std::size_t at = 0;

            if (it == records.mByPosition.end())
            {
                at = records.mValues.size();
                records.mByPosition.emplace_hint(it, position, at);
                records.mValues.emplace_back(deleted, std::move(record));
            }
            else
            {
                at = it->second;
                Record<ESM::Cell>& old = records.mValues[at];
                old.mValue.mData = record.mData;

                // **The name too, which the merge used to drop.** An entry standing here may be one
                // `exteriorAt` invented for a reference moved into it, and that one has no name at
                // all until the file that authored the cell arrives.
                old.mValue.mName = record.mName;
            }

            // **Between the two halves of loading a cell, which is the only place it fits.** The walk
            // starts where the reference block starts and `postLoad` is what records that position,
            // so the moved references are read before it and the reader put back afterwards. Anything
            // that reallocated `mValues` in between is why every step re-indexes.
            records.mValues[at].mValue.loadCell(reader, false);
            handleMovedRefs(reader, at, records);
            records.mValues[at].mValue.postLoad(reader);
        }

        template <class T>
        struct AsShallow;

        template <class... Ts>
        struct AsShallow<std::tuple<Ts...>>
        {
            using Type = std::tuple<Records<Ts>...>;
        };

        /// The shallow half of `ModelStore`: every model-bearing type as it comes off the reader,
        /// still carrying the deletion flags and the duplicates that merging will resolve.
        using ShallowModels = AsShallow<ModelRecords>::Type;

        constexpr auto sModelIndices = std::make_index_sequence<std::tuple_size_v<ModelRecords>>{};

        void loadRecord(ESM::ESMReader& reader, std::vector<LandTextureRecord>& records)
        {
            ESM::LandTexture value;
            bool deleted = false;
            value.load(reader, deleted);

            records.push_back(LandTextureRecord{
                .mId = std::move(value.mId),
                .mIndex = value.mIndex,
                .mTexture = value.mTexture.getNormalized(),
                .mPlugin = reader.getIndex(),
                .mDeleted = deleted,
            });
        }

        struct ShallowContent
        {
            CellRecords mCells;
            Records<ESM::GameSetting> mGameSettings;
            Records<ESM::Land> mLands;
            Records<ESM::Region> mRegions;
            std::vector<LandTextureRecord> mLandTextures;
            ShallowModels mModels;
        };

        /// Reads the record into the `i`th model table if that is the one `name` belongs to.
        ///
        /// Returns false both when the type does not match and when it matches a type the query did
        /// not ask for, because either way this record is one for the caller to skip.
        template <std::size_t i>
        bool loadModelRecord(const Query& query, const ESM::NAME& name, ESM::ESMReader& reader, ShallowContent& content)
        {
            using T = std::tuple_element_t<i, ModelRecords>;
            if (name.toInt() != T::sRecordId || !query.mModels.test(i))
                return false;

            loadRecord(reader, std::get<i>(content.mModels));
            return true;
        }

        template <std::size_t... i>
        bool loadModelRecord(const Query& query, const ESM::NAME& name, ESM::ESMReader& reader, ShallowContent& content,
            std::index_sequence<i...>)
        {
            return (loadModelRecord<i>(query, name, reader, content) || ...);
        }

        void loadRecord(const Query& query, const ESM::NAME& name, ESM::ESMReader& reader, ShallowContent& content)
        {
            switch (name.toInt())
            {
                case ESM::REC_CELL:
                    if (query.mLoadCells)
                        return loadRecord(reader, content.mCells);
                    break;
                case ESM::REC_GMST:
                    if (query.mLoadGameSettings)
                        return loadRecord(reader, content.mGameSettings);
                    break;
                case ESM::REC_LAND:
                    if (query.mLoadLands)
                        return loadRecord(reader, content.mLands);
                    break;
                case ESM::REC_REGN:
                    if (query.mLoadRegions)
                        return loadRecord(reader, content.mRegions);
                    break;
                case ESM::REC_LTEX:
                    if (query.mLoadLandTextures)
                        return loadRecord(reader, content.mLandTextures);
                    break;
                default:
                    if (loadModelRecord(query, name, reader, content, sModelIndices))
                        return;
                    break;
            }

            reader.skipRecord();
        }

        void loadEsm(const Query& query, ESM::ESMReader& reader, ShallowContent& content, Loading::Listener* listener)
        {
            Log(Debug::Info) << "Loading ESM file " << reader.getName();

            while (reader.hasMoreRecs())
            {
                const ESM::NAME recName = reader.getRecName();
                reader.getRecHeader();
                if (reader.getRecordFlags() & ESM::FLAG_Ignored)
                {
                    reader.skipRecord();
                    continue;
                }
                loadRecord(query, recName, reader, content);

                if (listener != nullptr)
                    listener->setProgress(fileProgress * reader.getFileOffset() / reader.getFileSize());
            }
        }

        ShallowContent shallowLoad(const Query& query, const std::vector<std::string>& contentFiles,
            const Files::Collections& fileCollections, ESM::ReadersCache& readers, ToUTF8::Utf8Encoder* encoder,
            Loading::Listener* listener)
        {
            ShallowContent result;

            const std::set<std::string_view, Misc::StringUtils::CiComp> supportedFormats{
                "esm",
                "esp",
                "omwgame",
                "omwaddon",
                "project",
            };

            for (std::size_t i = 0; i < contentFiles.size(); ++i)
            {
                const std::string& file = contentFiles[i];
                const std::string_view extension = Misc::getFileExtension(file);

                if (!supportedFormats.contains(extension))
                {
                    Log(Debug::Warning) << "Skipping unsupported content file: " << file;
                    continue;
                }

                if (listener != nullptr)
                {
                    listener->setLabel(file);
                    listener->setProgressRange(fileProgress);
                }

                const Files::MultiDirCollection& collection = fileCollections.getCollection(extension);

                const ESM::ReadersCache::BusyItem reader = readers.get(i);
                reader->setEncoder(encoder);
                reader->setIndex(static_cast<int>(i));
                reader->open(collection.getPath(file));
                if (query.mLoadCells)
                    reader->resolveParentFileIndices(readers);

                loadEsm(query, *reader, result, listener);
            }

            return result;
        }

        struct WithType
        {
            ESM::RecNameInts mType;

            template <class T>
            RefIdWithType operator()(const T& v) const
            {
                return { v.mId, mType };
            }
        };

        template <class T>
        void addRefIdsTypes(const std::vector<T>& values, std::vector<RefIdWithType>& refIdsTypes)
        {
            std::transform(values.begin(), values.end(), std::back_inserter(refIdsTypes),
                WithType{ static_cast<ESM::RecNameInts>(T::sRecordId) });
        }

        void addRefIdsTypes(EsmData& content)
        {
            std::size_t total = 0;
            std::apply([&](const auto&... records) { ((total += records.size()), ...); }, content.mModels);
            content.mRefIdTypes.reserve(total);

            std::apply(
                [&](const auto&... records) { (addRefIdsTypes(records, content.mRefIdTypes), ...); }, content.mModels);

            std::sort(content.mRefIdTypes.begin(), content.mRefIdTypes.end(), LessById{});
        }

        /// Names a record type and how many of it there were, or says nothing when there were none.
        ///
        /// Silent on the empty ones because a query naming seventeen types and finding three of them
        /// would otherwise print fourteen zeroes and bury the answer.
        template <class T>
        void reportCount(std::ostringstream& out, std::size_t count)
        {
            if (count != 0)
                out << ' ' << count << ' ' << T::getRecordType() << ',';
        }

        template <class Tuple, std::size_t... i>
        void reportModels(std::ostringstream& out, const Tuple& models, std::index_sequence<i...>)
        {
            (reportCount<std::tuple_element_t<i, ModelRecords>>(out, std::get<i>(models).size()), ...);
        }

        template <std::size_t... i>
        void prepareModels(ShallowModels& shallow, ModelStore& prepared, std::index_sequence<i...>)
        {
            ((std::get<i>(prepared) = prepareRecords(std::get<i>(shallow), GetKey{})), ...);
        }

        std::vector<ESM::Cell> prepareCellRecords(Records<ESM::Cell>& records)
        {
            std::vector<ESM::Cell> result;
            for (Record<ESM::Cell>& v : records)
                if (!v.mDeleted)
                    result.emplace_back(std::move(v.mValue));
            return result;
        }
    }

    EsmData loadEsmData(const Query& query, const std::vector<std::string>& contentFiles,
        const Files::Collections& fileCollections, ESM::ReadersCache& readers, ToUTF8::Utf8Encoder* encoder,
        Loading::Listener* listener)
    {
        Log(Debug::Info) << "Loading ESM data...";

        ShallowContent content = shallowLoad(query, contentFiles, fileCollections, readers, encoder, listener);

        std::ostringstream loaded;

        if (query.mLoadCells)
            loaded << ' ' << content.mCells.mValues.size() << " cells,";
        if (query.mLoadGameSettings)
            loaded << ' ' << content.mGameSettings.size() << " game settings,";
        if (query.mLoadLands)
            loaded << ' ' << content.mLands.size() << " lands,";
        if (query.mLoadLandTextures)
            loaded << ' ' << content.mLandTextures.size() << " land textures,";
        if (query.mLoadRegions)
            loaded << ' ' << content.mRegions.size() << " regions,";
        reportModels(loaded, content.mModels, sModelIndices);

        Log(Debug::Info) << "Loaded" << loaded.str();

        EsmData result;

        if (query.mLoadCells)
            result.mCells = prepareCellRecords(content.mCells.mValues);
        if (query.mLoadGameSettings)
            result.mGameSettings = prepareRecords(content.mGameSettings, GetKey{});
        if (query.mLoadLands)
            result.mLands = prepareRecords(content.mLands, GetKey{});
        result.mLandTextures = prepareLandTextures(content.mLandTextures);
        if (query.mLoadRegions)
            result.mRegions = prepareRecords(content.mRegions, GetKey{});
        prepareModels(content.mModels, result.mModels, sModelIndices);

        addRefIdsTypes(result);

        std::ostringstream prepared;

        if (query.mLoadCells)
            prepared << ' ' << result.mCells.size() << " cells,";
        if (query.mLoadGameSettings)
            prepared << ' ' << result.mGameSettings.size() << " game settings,";
        if (query.mLoadLands)
            prepared << ' ' << result.mLands.size() << " lands,";
        if (query.mLoadLandTextures)
            prepared << ' ' << result.mLandTextures.size() << " land textures,";
        if (query.mLoadRegions)
            prepared << ' ' << result.mRegions.size() << " regions,";
        reportModels(prepared, result.mModels, sModelIndices);

        Log(Debug::Info) << "Merged across content files to" << prepared.str();

        return result;
    }
}
