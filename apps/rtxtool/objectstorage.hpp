#pragma once

#include <map>
#include <utility>

#include <components/esm3/refnum.hpp>
#include <components/terrain/objectstorage.hpp>

namespace ESM
{
    struct Cell;
}

namespace EsmLoader
{
    struct EsmData;
}

namespace RtxTool
{
    /// What the content files say stands where, read with no game running behind them.
    ///
    /// **The second implementation of the seam, and the reason there is one.** The paging that
    /// builds a distant hillside is a component; what it reads the world out of is not, and the two
    /// worlds that read it — a running game and this — hold their records in different containers.
    /// Everything above this answers identically for both, which is the whole point of the harness.
    class ObjectStorage final : public Terrain::ObjectStorage
    {
    public:
        explicit ObjectStorage(const EsmLoader::EsmData& content);

        void collectReferences(float size, const osg::Vec2i& startCell, ESM::RefId worldspace,
            std::map<ESM::RefNum, Terrain::PagedCellRef>& out) const override;

        VFS::Path::Normalized getModel(int type, const ESM::RefId& id) const override;

        int getEsmVersion(int contentFile) const override;

    private:
        const EsmLoader::EsmData* mContent;

        /// The exteriors by grid position, built once.
        ///
        /// **Because a chunk asks for a square of them.** The content files are a flat list of two
        /// thousand cells with no order this can bisect, and a chunk sixty-four cells wide would
        /// scan it four thousand times to find what stands in it.
        std::map<std::pair<int, int>, const ESM::Cell*> mExteriors;
    };
}
