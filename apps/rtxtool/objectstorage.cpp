#include "objectstorage.hpp"

#include <algorithm>
#include <cassert>

#include <components/debug/debuglog.hpp>
#include <components/esm3/cellref.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm3/readerscache.hpp>
#include <components/esmloader/esmdata.hpp>
#include <components/esmloader/lessbyid.hpp>

namespace RtxTool
{
    ObjectStorage::ObjectStorage(const EsmLoader::EsmData& content)
        : mContent(&content)
    {
        for (const ESM::Cell& cell : content.mCells)
            if (cell.isExterior())
                mExteriors.emplace(std::pair(cell.getGridX(), cell.getGridY()), &cell);
    }

    void ObjectStorage::collectReferences(float size, const osg::Vec2i& startCell, ESM::RefId worldspace,
        std::map<ESM::RefNum, Terrain::PagedCellRef>& out) const
    {
        // **Said rather than answered with an empty hillside.** `EsmLoader` reads ESM3 and this
        // world builds one worldspace; a chunk asked for another would silently come back bare,
        // which reads exactly like the bug this class was written to fix.
        assert(worldspace == ESM::Cell::sDefaultWorldspaceId);

        out.clear();

        const auto typeOf = [&](const ESM::RefId& id) {
            const auto found = std::lower_bound(
                mContent->mRefIdTypes.begin(), mContent->mRefIdTypes.end(), id, EsmLoader::LessById{});
            return found == mContent->mRefIdTypes.end() || found->mId != id ? 0 : static_cast<int>(found->mType);
        };

        // **Its own, because chunks are built on the paging's working threads.** A cache shared with
        // the caller would be two threads seeking one file handle. The game's implementation does
        // the same, for the same reason.
        ESM::ReadersCache readers;

        for (int cellX = startCell.x(); cellX < startCell.x() + size; ++cellX)
        {
            for (int cellY = startCell.y(); cellY < startCell.y() + size; ++cellY)
            {
                const auto found = mExteriors.find(std::pair(cellX, cellY));
                if (found == mExteriors.end())
                    continue;

                const ESM::Cell& cell = *found->second;

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
                            if (moved)
                                continue;

                            const int recordType = typeOf(ref.mRefID);
                            if (!Terrain::pagedType(recordType, size >= 2))
                                continue;
                            if (deleted || departed(ref.mRefNum))
                            {
                                out.erase(ref.mRefNum);
                                continue;
                            }

                            out.insert_or_assign(ref.mRefNum,
                                Terrain::PagedCellRef{
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
                    if (!Terrain::pagedType(recordType, size >= 2))
                        continue;

                    out.insert_or_assign(leased.mRefNum,
                        Terrain::PagedCellRef{
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

    VFS::Path::Normalized ObjectStorage::getModel(int type, const ESM::RefId& id) const
    {
        return VFS::Path::Normalized(EsmLoader::getModel(*mContent, id, static_cast<ESM::RecNameInts>(type)));
    }

    int ObjectStorage::getEsmVersion(int /*contentFile*/) const
    {
        // **Nothing, which means no file gets its distant mesh** — `getDistantMeshPattern` reads a
        // version to choose between `_dist`, `_far` and `_lod`, and `EsmLoader` does not record
        // which Morrowind wrote a content file.
        //
        // **Measured rather than waved away.** Forcing this to zero leaves a four-cell Balmora
        // byte-identical at 2,252,922 triangles: vanilla, Tribunal and Bloodmoon ship no such
        // meshes, so the lookup falls back to the full model every time. Content that does ship
        // them would come out heavier here than in the game, and this fork does not target it.
        return 0;
    }
}
