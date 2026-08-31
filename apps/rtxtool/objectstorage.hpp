#pragma once

#include <functional>
#include <map>
#include <optional>

#include <components/esm3/refnum.hpp>
#include <components/terrain/objectstorage.hpp>

#include "exteriorindex.hpp"

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
        /// @param exteriors where a square is, which this asks for every cell a chunk covers. Read
        ///        rather than built here, because the harness asks the same question at a crossing
        ///        and the two must not be two answers.
        ObjectStorage(const EsmLoader::EsmData& content, const ExteriorIndex& exteriors);

        void collectReferences(float size, const osg::Vec2i& startCell, ESM::RefId worldspace,
            std::map<ESM::RefNum, Terrain::PagedCellRef>& out) const override;

        void collectLights(float size, const osg::Vec2i& startCell, ESM::RefId worldspace,
            std::map<ESM::RefNum, Terrain::PagedCellRef>& out) const override;

        std::optional<SceneUtil::LightCommon> getLight(const ESM::RefId& id) const override;

        VFS::Path::Normalized getModel(int type, const ESM::RefId& id) const override;

        int getEsmVersion(int contentFile) const override;

    private:
        /// The one reading, asked the caller's question. What differs between the two collectors is
        /// a predicate; what must not differ is which blocks are read and how a later content file
        /// wins over an earlier one.
        void collect(float size, const osg::Vec2i& startCell, ESM::RefId worldspace,
            const std::function<bool(int, bool)>& wanted, std::map<ESM::RefNum, Terrain::PagedCellRef>& out) const;

        const EsmLoader::EsmData* mContent;
        const ExteriorIndex* mExteriors;
    };
}
