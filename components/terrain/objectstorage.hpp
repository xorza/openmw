#ifndef OPENMW_COMPONENTS_TERRAIN_OBJECTSTORAGE_H
#define OPENMW_COMPONENTS_TERRAIN_OBJECTSTORAGE_H

#include <functional>
#include <map>
#include <optional>

#include <osg/Vec2i>
#include <osg/Vec3f>

#include <components/esm/defs.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/refnum.hpp>
#include <components/sceneutil/lightcommon.hpp>
#include <components/vfs/pathutil.hpp>

namespace ESM
{
    struct Cell;
}

namespace Terrain
{
    /// One reference a chunk stands, reduced to what the paging needs of it.
    struct PagedCellRef
    {
        ESM::RefId mRefId;
        ESM::RefNum mRefNum;
        osg::Vec3f mPosition;
        osg::Vec3f mRotation;
        float mScale = 1.f;

        /// The record type `mRefId` resolves to. Carried rather than looked up again: deciding
        /// whether the reference pages at all already needed it.
        int mType = 0;
    };

    /// Whether a record type is paged at all, and whether it is still paged once a chunk is wider
    /// than a cell.
    ///
    /// **One answer, because it decides what a distant hillside is made of.** A world that pages
    /// containers and one that does not are two different pictures; an implementation of the storage
    /// below that filtered by its own reckoning would be a second opinion, which is exactly what
    /// having an interface here is meant to prevent.
    inline bool pagedType(int type, bool far)
    {
        switch (type)
        {
            case ESM::REC_STAT:
            case ESM::REC_ACTI:
            case ESM::REC_DOOR:
            case ESM::REC_STAT4:
            case ESM::REC_DOOR4:
            case ESM::REC_TREE4:
                return true;
            case ESM::REC_CONT:
            case ESM::REC_ACTI4:
            case ESM::REC_CONT4:
            case ESM::REC_FURN4:
                return !far;

            default:
                return false;
        }
    }

    /// Whether a record type carries a light the paging leaves behind.
    ///
    /// **Not part of `pagedType`, and never to be merged into it.** That one decides what a distant
    /// hillside is *made of*, both renderers read it, and `REC_LIGH` is deliberately absent — so
    /// upstream stands no distant lantern and a renderer that started standing one would be showing
    /// a different world. This asks a different question of the same references: which of them carry
    /// light that nobody has placed, because the model they hang on was never paged.
    ///
    /// Takes `pagedType`'s two arguments so that either may be handed to `collectPagedRefs`. A light
    /// is a light however wide the chunk is.
    inline bool litType(int type, bool /*far*/)
    {
        return type == ESM::REC_LIGH;
    }

    /// Every reference that pages in a square of ESM3 exterior cells, merged into `out`.
    ///
    /// **The reduction both worlds read one hillside through.** A later content file can move,
    /// delete or add to what an earlier one placed, and only that file says so — so the blocks are
    /// read in the order they were written and merged by reference number before anything is drawn.
    /// Two spellings of that is one world with a building the other has not got.
    ///
    /// `out` is added to rather than cleared, so a caller reading more than one worldspace keeps
    /// what the others found.
    ///
    /// @param cellAt the cell at a grid position, or null where the content files define none —
    ///        open sea, which is skipped rather than missing.
    /// @param typeOf what record a reference names, which the two worlds answer out of different
    ///        stores. Read through `std::function` because this runs once per chunk built and
    ///        never on a frame.
    /// @param wanted which record types the caller is asking about — `pagedType` for what a chunk
    ///        stands, `litType` for what lights it. **The caller's and not this function's**: the
    ///        two questions read the same blocks in the same order, and the reduction by reference
    ///        number that makes a later content file win has to be the same one for both.
    void collectPagedRefs(float size, const osg::Vec2i& startCell,
        const std::function<const ESM::Cell*(int, int)>& cellAt, const std::function<int(const ESM::RefId&)>& typeOf,
        const std::function<bool(int, bool)>& wanted, std::map<ESM::RefNum, PagedCellRef>& out);

    /// What the paging and the ray tracer ask of the content files.
    ///
    /// **The seam `Terrain::Storage` already is, for the same reason.** The paging is a thousand
    /// lines of scene-graph work — load, merge, analyse, batch — and about forty of reading records,
    /// and it was those forty that tied it to a running game. Behind this the two are the same code:
    /// a harness that stands a hillside up and the game that draws it cannot answer differently
    /// about what is on it.
    class ObjectStorage
    {
    public:
        virtual ~ObjectStorage() = default;

        /// Every reference that pages in the square of `size` cells whose lowest corner is
        /// `startCell`, reduced by reference number the way the content files stack: a later file
        /// moving or deleting what an earlier one placed wins.
        ///
        /// `out` is cleared first. Called from the paging's own working threads, so an
        /// implementation must be safe to call on several at once.
        virtual void collectReferences(float size, const osg::Vec2i& startCell, ESM::RefId worldspace,
            std::map<ESM::RefNum, PagedCellRef>& out) const = 0;

        /// Every `LIGH` reference in that same square, reduced the same way.
        ///
        /// **The one thing the paging does not stand, asked for separately.** `pagedType` leaves
        /// `REC_LIGH` out, so a distant lantern has no model in either renderer — but the ray tracer
        /// lights the world with what it can reach rather than with what a camera can see, and a
        /// town four cells away that goes dark at dusk is the world stating something the content
        /// files do not.
        ///
        /// `out` is cleared first. Called from a worker thread, so an implementation must be safe to
        /// call on several at once.
        virtual void collectLights(float size, const osg::Vec2i& startCell, ESM::RefId worldspace,
            std::map<ESM::RefNum, PagedCellRef>& out) const = 0;

        /// What a `LIGH` record says its light is, or nothing where the id names no such record.
        ///
        /// **`SceneUtil::LightCommon` and not a shape of this fork's own**, because that is what
        /// `SceneUtil::createLightSource` takes and what every light the game places is built out
        /// of. A second reading of the same eight fields is a second answer waiting to drift.
        virtual std::optional<SceneUtil::LightCommon> getLight(const ESM::RefId& id) const = 0;

        /// The model a record names, or empty where it names none — a marker, or a type that draws
        /// nothing.
        virtual VFS::Path::Normalized getModel(int type, const ESM::RefId& id) const = 0;

        /// Which Morrowind wrote the content file a reference came from.
        ///
        /// **What names a distant mesh.** `_dist`, `_far` and `_lod` are three spellings of the same
        /// idea and the version is the only thing that says which one a file uses, so a world that
        /// cannot answer this draws full-detail models where the game draws the cheap ones.
        virtual int getEsmVersion(int contentFile) const = 0;
    };
}

#endif
