#include "objectstorage.hpp"

#include <algorithm>
#include <cassert>

#include <components/esm3/loadcell.hpp>
#include <components/esmloader/esmdata.hpp>
#include <components/esmloader/lessbyid.hpp>

namespace RtxTool
{
    ObjectStorage::ObjectStorage(const EsmLoader::EsmData& content, const ExteriorIndex& exteriors)
        : mContent(&content)
        , mExteriors(&exteriors)
    {
    }

    void ObjectStorage::collectReferences(float size, const osg::Vec2i& startCell, ESM::RefId worldspace,
        std::map<ESM::RefNum, Terrain::PagedCellRef>& out) const
    {
        // **Said rather than answered with an empty hillside.** `EsmLoader` reads ESM3 and this
        // world builds one worldspace; a chunk asked for another would silently come back bare,
        // which reads exactly like the bug this class was written to fix.
        assert(worldspace == ESM::Cell::sDefaultWorldspaceId);

        out.clear();

        Terrain::collectPagedRefs(
            size, startCell, [&](int x, int y) { return mExteriors->find(x, y); },
            [&](const ESM::RefId& id) {
                const auto found = std::lower_bound(
                    mContent->mRefIdTypes.begin(), mContent->mRefIdTypes.end(), id, EsmLoader::LessById{});
                return found == mContent->mRefIdTypes.end() || found->mId != id ? 0 : static_cast<int>(found->mType);
            },
            out);
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
