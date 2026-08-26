#include "objectstorage.hpp"

#include <algorithm>

#include <components/debug/debuglog.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/loadacti.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm3/loadcont.hpp>
#include <components/esm3/loaddoor.hpp>
#include <components/esm3/loadstat.hpp>
#include <components/esm3/readerscache.hpp>
#include <components/esm4/loadacti.hpp>
#include <components/esm4/loadcont.hpp>
#include <components/esm4/loaddoor.hpp>
#include <components/esm4/loadfurn.hpp>
#include <components/esm4/loadstat.hpp>
#include <components/esm4/loadtree.hpp>

#include "apps/openmw/mwbase/environment.hpp"
#include "apps/openmw/mwbase/world.hpp"
#include "apps/openmw/mwclass/esm4base.hpp"
#include "apps/openmw/mwworld/esmstore.hpp"

namespace MWRender
{
    namespace
    {
        template <typename Record>
        VFS::Path::Normalized getEsm4Model(const Record& record)
        {
            if (MWClass::ESM4Impl::isMarkerModel(record->mModel.getOriginal()))
                return {};
            return record->mModel.getNormalized();
        }

        Terrain::PagedCellRef makePagedCellRef(const ESM::CellRef& value, int type)
        {
            return Terrain::PagedCellRef{
                .mRefId = value.mRefID,
                .mRefNum = value.mRefNum,
                .mPosition = value.mPos.asVec3(),
                .mRotation = value.mPos.asRotationVec3(),
                .mScale = value.mScale,
                .mType = type,
            };
        }

        Terrain::PagedCellRef makePagedCellRef(const ESM4::Reference& value, int type)
        {
            return Terrain::PagedCellRef{
                .mRefId = value.mBaseObj,
                .mRefNum = value.mId,
                .mPosition = value.mPos.asVec3(),
                .mRotation = value.mPos.asRotationVec3(),
                .mScale = value.mScale,
                .mType = type,
            };
        }

        void collectESM3References(float size, const osg::Vec2i& startCell, const MWWorld::ESMStore& store,
            std::map<ESM::RefNum, Terrain::PagedCellRef>& refs)
        {
            ESM::ReadersCache readers;
            for (int cellX = startCell.x(); cellX < startCell.x() + size; ++cellX)
            {
                for (int cellY = startCell.y(); cellY < startCell.y() + size; ++cellY)
                {
                    const ESM::Cell* cell = store.get<ESM::Cell>().searchStatic(cellX, cellY);
                    if (!cell)
                        continue;
                    for (size_t i = 0; i < cell->mContextList.size(); ++i)
                    {
                        try
                        {
                            const std::size_t index = static_cast<std::size_t>(cell->mContextList[i].index);
                            const ESM::ReadersCache::BusyItem reader = readers.get(index);
                            cell->restore(*reader, i);
                            ESM::CellRef ref;
                            ESM::MovedCellRef cMRef;
                            bool deleted = false;
                            bool moved = false;
                            while (ESM::Cell::getNextRef(
                                *reader, ref, deleted, cMRef, moved, ESM::Cell::GetNextRefMode::LoadOnlyNotMoved))
                            {
                                if (moved)
                                    continue;

                                if (std::find(cell->mMovedRefs.begin(), cell->mMovedRefs.end(), ref.mRefNum)
                                    != cell->mMovedRefs.end())
                                    continue;

                                int type = store.findStatic(ref.mRefID);
                                if (!Terrain::pagedType(type, size >= 2))
                                    continue;
                                if (deleted)
                                {
                                    refs.erase(ref.mRefNum);
                                    continue;
                                }
                                refs.insert_or_assign(ref.mRefNum, makePagedCellRef(ref, type));
                            }
                        }
                        catch (const std::exception& e)
                        {
                            Log(Debug::Warning) << "Failed to collect references from cell \"" << cell->getDescription()
                                                << "\": " << e.what();
                            continue;
                        }
                    }
                    for (const auto& [ref, deleted] : cell->mLeasedRefs)
                    {
                        if (deleted)
                        {
                            refs.erase(ref.mRefNum);
                            continue;
                        }
                        int type = store.findStatic(ref.mRefID);
                        if (!Terrain::pagedType(type, size >= 2))
                            continue;
                        refs.insert_or_assign(ref.mRefNum, makePagedCellRef(ref, type));
                    }
                }
            }
        }

        void collectESM4References(float size, const osg::Vec2i& startCell, ESM::RefId worldspace,
            const MWWorld::ESMStore& store, std::map<ESM::RefNum, Terrain::PagedCellRef>& refs)
        {
            for (int cellX = startCell.x(); cellX < startCell.x() + size; ++cellX)
            {
                for (int cellY = startCell.y(); cellY < startCell.y() + size; ++cellY)
                {
                    const ESM4::Cell* cell
                        = store.get<ESM4::Cell>().searchExterior(ESM::ExteriorCellLocation(cellX, cellY, worldspace));
                    if (!cell)
                        continue;
                    for (const ESM4::Reference* ref4 : store.get<ESM4::Reference>().getByCell(cell->mId))
                    {
                        if (ref4->mFlags & ESM4::Rec_Disabled)
                            continue;
                        int type = store.findStatic(ref4->mBaseObj);
                        if (!Terrain::pagedType(type, size >= 2))
                            continue;
                        if (!ref4->mEsp.parent.isZeroOrUnset())
                        {
                            const ESM4::Reference* parentRef
                                = store.get<ESM4::Reference>().searchStatic(ref4->mEsp.parent);
                            if (parentRef)
                            {
                                bool parentDisabled = parentRef->mFlags & ESM4::Rec_Disabled;
                                bool inversed = ref4->mEsp.flags & ESM4::EnableParent::Flag_Inversed;
                                if (parentDisabled != inversed)
                                    continue;
                            }
                        }
                        refs.insert_or_assign(ref4->mId, makePagedCellRef(*ref4, type));
                    }
                }
            }
        }
    }

    void ObjectStorage::collectReferences(float size, const osg::Vec2i& startCell, ESM::RefId worldspace,
        std::map<ESM::RefNum, Terrain::PagedCellRef>& out) const
    {
        out.clear();

        const MWWorld::ESMStore& store = MWBase::Environment::get().getWorld()->getStore();

        if (worldspace == ESM::Cell::sDefaultWorldspaceId)
            collectESM3References(size, startCell, store, out);
        else
            collectESM4References(size, startCell, worldspace, store, out);
    }

    VFS::Path::Normalized ObjectStorage::getModel(int type, const ESM::RefId& id) const
    {
        const MWWorld::ESMStore& store = MWBase::Environment::get().getWorld()->getStore();

        switch (type)
        {
            case ESM::REC_STAT:
                return store.get<ESM::Static>().searchStatic(id)->mModel.getNormalized();
            case ESM::REC_ACTI:
                return store.get<ESM::Activator>().searchStatic(id)->mModel.getNormalized();
            case ESM::REC_DOOR:
                return store.get<ESM::Door>().searchStatic(id)->mModel.getNormalized();
            case ESM::REC_CONT:
                return store.get<ESM::Container>().searchStatic(id)->mModel.getNormalized();
            case ESM::REC_STAT4:
                return getEsm4Model(store.get<ESM4::Static>().searchStatic(id));
            case ESM::REC_DOOR4:
                return getEsm4Model(store.get<ESM4::Door>().searchStatic(id));
            case ESM::REC_TREE4:
                return getEsm4Model(store.get<ESM4::Tree>().searchStatic(id));
            case ESM::REC_ACTI4:
                return getEsm4Model(store.get<ESM4::Activator>().searchStatic(id));
            case ESM::REC_CONT4:
                return getEsm4Model(store.get<ESM4::Container>().searchStatic(id));
            case ESM::REC_FURN4:
                return getEsm4Model(store.get<ESM4::Furniture>().searchStatic(id));
            default:
                return {};
        }
    }

    int ObjectStorage::getEsmVersion(int contentFile) const
    {
        return MWBase::Environment::get().getWorld()->getESMVersions()[contentFile];
    }
}
