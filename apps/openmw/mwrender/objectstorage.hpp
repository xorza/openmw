#ifndef GAME_RENDER_OBJECTSTORAGE_H
#define GAME_RENDER_OBJECTSTORAGE_H

#include <components/terrain/objectstorage.hpp>

namespace MWRender
{
    /// What the running game's content files say stands where: the walk `ObjectPaging` reads its
    /// chunks through, offered to a renderer that stands the distance itself. Holds nothing; every
    /// answer comes out of `MWBase::Environment`. Defined in `objectpaging.cpp`, beside the walk.
    class ObjectStorage final : public Terrain::ObjectStorage
    {
    public:
        void collect(float size, const osg::Vec2i& startCell, ESM::RefId worldspace, Terrain::RefKinds kinds,
            std::vector<Terrain::PagedCellRef>& into) const override;

        std::optional<SceneUtil::LightCommon> getLight(const ESM::RefId& id) const override;

        VFS::Path::Normalized getModel(const ESM::RefId& id) const override;
    };
}

#endif
