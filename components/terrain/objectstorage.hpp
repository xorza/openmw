#ifndef OPENMW_COMPONENTS_TERRAIN_OBJECTSTORAGE_H
#define OPENMW_COMPONENTS_TERRAIN_OBJECTSTORAGE_H

#include <optional>
#include <vector>

#include <osg/Vec2i>
#include <osg/Vec3f>

#include <components/esm/refid.hpp>
#include <components/esm3/refnum.hpp>
#include <components/sceneutil/lightcommon.hpp>
#include <components/vfs/pathutil.hpp>

namespace Terrain
{
    /// One reference a chunk stands, reduced to what the paging needs of it.
    struct PagedCellRef
    {
        ESM::RefId mRefId{};
        ESM::RefNum mRefNum{};
        osg::Vec3f mPosition{};
        osg::Vec3f mRotation{};
        float mScale = 1.f;
    };

    /// Which references one walk of a cell's records collects, as a set of the two kinds.
    enum class RefKinds : unsigned int
    {
        /// What a chunk stands: the record types the paging draws, which is what makes a distant
        /// hillside look the same under both renderers.
        Paged = 1 << 0,

        /// What lights it and nothing stands: `LIGH`, which the paging never draws. The ray tracer
        /// lights the world with what it can reach rather than with what a camera can see, and a
        /// town four cells away that goes dark at dusk is the world stating something the content
        /// files do not.
        Lit = 1 << 1,

        Both = Paged | Lit,
    };

    constexpr bool holds(RefKinds set, RefKinds one)
    {
        return (static_cast<unsigned int>(set) & static_cast<unsigned int>(one)) != 0;
    }

    /// What the paging and the ray tracer ask of the content files.
    ///
    /// **The seam `Terrain::Storage` already is, for the same reason.** The paging is a thousand
    /// lines of scene-graph work and about forty of reading records, and it was those forty that
    /// tied it to a running game. Behind this the two are the same code: a harness that stands a
    /// hillside up and the game that draws it cannot answer differently about what is on it.
    class ObjectStorage
    {
    public:
        virtual ~ObjectStorage() = default;

        /// Every reference of the `kinds` asked for in the square of `size` cells whose lowest
        /// corner is `startCell`, reduced by reference number the way the content files stack: a
        /// later file moving or deleting what an earlier one placed wins. Sorted by reference
        /// number. Both kinds come from one walk, because a walk opens the cell's readers and a
        /// second walk for the other kind opened them again; `getLight` says which kind a
        /// reference is.
        ///
        /// `into` is cleared first. Called from the paging's own working threads, so an
        /// implementation must be safe to call on several at once.
        virtual void collect(float size, const osg::Vec2i& startCell, ESM::RefId worldspace, RefKinds kinds,
            std::vector<PagedCellRef>& into) const = 0;

        /// What a `LIGH` record says its light is, or nothing where the id names no such record.
        ///
        /// **`SceneUtil::LightCommon` and not a shape of this fork's own**, because that is what
        /// `SceneUtil::createLightSource` takes and what every light the game places is built out
        /// of. A second reading of the same eight fields is a second answer waiting to drift.
        virtual std::optional<SceneUtil::LightCommon> getLight(const ESM::RefId& id) const = 0;

        /// The model a reference's record names, or empty where it names none — a marker, or a
        /// type that draws nothing.
        virtual VFS::Path::Normalized getModel(const ESM::RefId& id) const = 0;
    };
}

#endif
