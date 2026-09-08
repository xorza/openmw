#include "objectstorage.hpp"

#include <algorithm>

#include <components/esm3/loadacti.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm3/loadcont.hpp>
#include <components/esm3/loaddoor.hpp>
#include <components/esm3/loadligh.hpp>
#include <components/esm3/loadstat.hpp>
#include <components/esm4/loadacti.hpp>
#include <components/esm4/loadcont.hpp>
#include <components/esm4/loaddoor.hpp>
#include <components/esm4/loadfurn.hpp>
#include <components/esm4/loadstat.hpp>
#include <components/esm4/loadtree.hpp>
#include <components/sceneutil/lightcommon.hpp>

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

        /// The running game's cells and record types, which is the whole of what a collection asks.
        class GameCells final : public Terrain::CellSource
        {
        public:
            explicit GameCells(const MWWorld::ESMStore& store)
                : mStore(store)
            {
            }

            const ESM::Cell* getCell(const int x, const int y) const override
            {
                return mStore.get<ESM::Cell>().searchStatic(x, y);
            }

            int getType(const ESM::RefId& id) const override { return mStore.findStatic(id); }

        private:
            const MWWorld::ESMStore& mStore;
        };

        void collectESM4References(float size, const osg::Vec2i& startCell, ESM::RefId worldspace,
            const MWWorld::ESMStore& store, std::vector<Terrain::PagedCellRef>& refs)
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
                        refs.push_back(makePagedCellRef(*ref4, type));
                    }
                }
            }

            // The order a reduction leaves, since nothing here says a reference twice.
            std::sort(refs.begin(), refs.end(),
                [](const Terrain::PagedCellRef& a, const Terrain::PagedCellRef& b) { return a.mRefNum < b.mRefNum; });
        }
    }

    void ObjectStorage::collect(Terrain::RefKind kind, float size, const osg::Vec2i& startCell, ESM::RefId worldspace,
        std::vector<Terrain::PagedCellRef>& out) const
    {
        out.clear();

        const MWWorld::ESMStore& store = MWBase::Environment::get().getWorld()->getStore();

        if (worldspace == ESM::Cell::sDefaultWorldspaceId)
        {
            const GameCells cells(store);
            Terrain::collectPagedRefs(size, startCell, cells, kind, out);
            return;
        }

        // **ESM3 alone for the lights, and empty rather than a failure for anything else.** A `LIGH`
        // is a Morrowind record and `collectESM4References` walks a different store with a different
        // reference shape; a worldspace out of ESM4 content keeps exactly the lighting it has today.
        if (kind == Terrain::RefKind::Paged)
            collectESM4References(size, startCell, worldspace, store, out);
    }

    std::optional<SceneUtil::LightCommon> ObjectStorage::getLight(const ESM::RefId& id) const
    {
        const MWWorld::ESMStore& store = MWBase::Environment::get().getWorld()->getStore();

        const ESM::Light* found = store.get<ESM::Light>().search(id);
        if (found == nullptr)
            return std::nullopt;

        return SceneUtil::LightCommon(*found);
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
