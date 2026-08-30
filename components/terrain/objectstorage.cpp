#include "objectstorage.hpp"

#include <algorithm>

#include <components/debug/debuglog.hpp>
#include <components/esm3/cellref.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm3/readerscache.hpp>

namespace Terrain
{
    void collectPagedRefs(float size, const osg::Vec2i& startCell,
        const std::function<const ESM::Cell*(int, int)>& cellAt, const std::function<int(const ESM::RefId&)>& typeOf,
        std::map<ESM::RefNum, PagedCellRef>& out)
    {
        // **Its own, because chunks are built on the paging's working threads.** A cache shared with
        // the caller would be two threads seeking one file handle.
        ESM::ReadersCache readers;

        for (int cellX = startCell.x(); cellX < startCell.x() + size; ++cellX)
        {
            for (int cellY = startCell.y(); cellY < startCell.y() + size; ++cellY)
            {
                const ESM::Cell* found = cellAt(cellX, cellY);
                if (found == nullptr)
                    continue;

                const ESM::Cell& cell = *found;

                // **What a later content file moved out of this cell.** Only that file carries the
                // `MVRF`, so a block written by an earlier one still stands the reference where it
                // used to be — and a chunk that merged it there would have a building in two places.
                const auto departed = [&](const ESM::RefNum& refNum) {
                    return std::find(cell.mMovedRefs.begin(), cell.mMovedRefs.end(), refNum) != cell.mMovedRefs.end();
                };

                for (std::size_t i = 0; i < cell.mContextList.size(); ++i)
                {
                    try
                    {
                        const ESM::ReadersCache::BusyItem reader
                            = readers.get(static_cast<std::size_t>(cell.mContextList[i].index));
                        cell.restore(*reader, static_cast<int>(i));

                        ESM::CellRef ref;
                        ESM::MovedCellRef movedRef;
                        bool deleted = false;
                        bool moved = false;
                        while (ESM::Cell::getNextRef(
                            *reader, ref, deleted, movedRef, moved, ESM::Cell::GetNextRefMode::LoadOnlyNotMoved))
                        {
                            if (moved || departed(ref.mRefNum))
                                continue;

                            const int recordType = typeOf(ref.mRefID);
                            if (!pagedType(recordType, size >= 2))
                                continue;
                            if (deleted)
                            {
                                out.erase(ref.mRefNum);
                                continue;
                            }

                            out.insert_or_assign(ref.mRefNum,
                                PagedCellRef{
                                    .mRefId = ref.mRefID,
                                    .mRefNum = ref.mRefNum,
                                    .mPosition = ref.mPos.asVec3(),
                                    .mRotation = ref.mPos.asRotationVec3(),
                                    .mScale = ref.mScale,
                                    .mType = recordType,
                                });
                        }
                    }
                    catch (const std::exception& e)
                    {
                        Log(Debug::Warning) << "Failed to collect references from cell \"" << cell.getDescription()
                                            << "\": " << e.what();
                        continue;
                    }
                }

                // **And what one moved in**, which this cell's own reference blocks never mention.
                for (const auto& [leased, deleted] : cell.mLeasedRefs)
                {
                    if (deleted)
                    {
                        out.erase(leased.mRefNum);
                        continue;
                    }

                    const int recordType = typeOf(leased.mRefID);
                    if (!pagedType(recordType, size >= 2))
                        continue;

                    out.insert_or_assign(leased.mRefNum,
                        PagedCellRef{
                            .mRefId = leased.mRefID,
                            .mRefNum = leased.mRefNum,
                            .mPosition = leased.mPos.asVec3(),
                            .mRotation = leased.mPos.asRotationVec3(),
                            .mScale = leased.mScale,
                            .mType = recordType,
                        });
                }
            }
        }
    }
}
