#ifndef OPENMW_COMPONENTS_TERRAIN_OBJECTSTORAGE_H
#define OPENMW_COMPONENTS_TERRAIN_OBJECTSTORAGE_H

#include <map>

#include <osg/Vec2i>
#include <osg/Vec3f>

#include <components/esm/defs.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/refnum.hpp>
#include <components/vfs/pathutil.hpp>

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

    /// What `ObjectPaging` asks of the content files.
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
