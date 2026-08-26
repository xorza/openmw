#ifndef GAME_RENDER_OBJECTSTORAGE_H
#define GAME_RENDER_OBJECTSTORAGE_H

#include <components/terrain/objectstorage.hpp>

namespace MWRender
{
    /// What the running game's content files say stands where.
    ///
    /// **Holds nothing.** Every answer comes out of `MWBase::Environment`, which is the game's own
    /// singleton — and which is exactly why `Terrain::ObjectPaging` used to need a world running
    /// behind it before it could build a hillside.
    class ObjectStorage final : public Terrain::ObjectStorage
    {
    public:
        void collectReferences(float size, const osg::Vec2i& startCell, ESM::RefId worldspace,
            std::map<ESM::RefNum, Terrain::PagedCellRef>& out) const override;

        VFS::Path::Normalized getModel(int type, const ESM::RefId& id) const override;

        int getEsmVersion(int contentFile) const override;
    };
}

#endif
