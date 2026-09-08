#include "objectstorage.hpp"

#include <algorithm>
#include <cstdint>

#include <components/debug/debuglog.hpp>
#include <components/esm3/cellref.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm3/readerscache.hpp>

namespace Terrain
{
    namespace
    {
        /// One reference as the blocks stated it, with what it takes to reduce the statements.
        ///
        /// **A flat list and a sort rather than a map keyed by reference number.** A chunk of a
        /// nine-cell square is several thousand references, and a node-based map spends an
        /// allocation on each — on a loading thread, for an answer that is read once in order.
        struct Statement
        {
            PagedCellRef mRef;

            /// Where in the reading this was said, so the last word about a reference number is the
            /// one the content files meant. `std::sort` is not stable, and this is what makes the
            /// order it cannot keep explicit.
            std::uint32_t mAt = 0;

            /// Whether the file said the reference is gone. A deletion is kept rather than applied
            /// as it is read, because what it deletes may not have been read yet: a later file can
            /// delete what an earlier one placed, and the blocks of one cell are read in file order.
            bool mDeleted = false;
        };

        void state(std::vector<Statement>& into, const PagedCellRef& ref, const bool deleted)
        {
            into.push_back(Statement{
                .mRef = ref,
                .mAt = static_cast<std::uint32_t>(into.size()),
                .mDeleted = deleted,
            });
        }

        /// Appends the last word about each reference number, in that order.
        void reduce(std::vector<Statement>& said, std::vector<PagedCellRef>& out)
        {
            std::sort(said.begin(), said.end(), [](const Statement& a, const Statement& b) {
                if (a.mRef.mRefNum < b.mRef.mRefNum)
                    return true;
                if (b.mRef.mRefNum < a.mRef.mRefNum)
                    return false;
                return a.mAt < b.mAt;
            });

            for (std::size_t at = 0; at < said.size(); ++at)
            {
                const bool lastOfRun = at + 1 == said.size() || !(said[at].mRef.mRefNum == said[at + 1].mRef.mRefNum);

                if (lastOfRun && !said[at].mDeleted)
                    out.push_back(said[at].mRef);
            }
        }
    }

    void collectPagedRefs(const float size, const osg::Vec2i& startCell, const CellSource& source, const RefKind kind,
        std::vector<PagedCellRef>& out)
    {
        // **Its own, because chunks are built on the paging's working threads.** A cache shared with
        // the caller would be two threads seeking one file handle.
        ESM::ReadersCache readers;

        std::vector<Statement> said;
        const bool far = size >= 2;

        for (int cellX = startCell.x(); cellX < startCell.x() + size; ++cellX)
        {
            for (int cellY = startCell.y(); cellY < startCell.y() + size; ++cellY)
            {
                const ESM::Cell* found = source.getCell(cellX, cellY);
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

                            const int recordType = source.getType(ref.mRefID);
                            if (!wantedType(kind, recordType, far))
                                continue;

                            state(said,
                                PagedCellRef{
                                    .mRefId = ref.mRefID,
                                    .mRefNum = ref.mRefNum,
                                    .mPosition = ref.mPos.asVec3(),
                                    .mRotation = ref.mPos.asRotationVec3(),
                                    .mScale = ref.mScale,
                                    .mType = recordType,
                                },
                                deleted);
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
                        state(said, PagedCellRef{ .mRefNum = leased.mRefNum }, true);
                        continue;
                    }

                    const int recordType = source.getType(leased.mRefID);
                    if (!wantedType(kind, recordType, far))
                        continue;

                    state(said,
                        PagedCellRef{
                            .mRefId = leased.mRefID,
                            .mRefNum = leased.mRefNum,
                            .mPosition = leased.mPos.asVec3(),
                            .mRotation = leased.mPos.asRotationVec3(),
                            .mScale = leased.mScale,
                            .mType = recordType,
                        },
                        false);
                }
            }
        }

        reduce(said, out);
    }
}
