#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include <boost/container/flat_set.hpp>
#include <osg/Image>
#include <osg/Node>
#include <osg/Vec2i>

#include <components/esm/refid.hpp>
#include <components/terrain/objectstorage.hpp>
#include <components/terrain/storage.hpp>
#include <components/vfs/pathutil.hpp>

#include "groundreader.hpp"
#include "prepared.hpp"
#include "scratch.hpp"
#include "slots.hpp"
#include "templatewalk.hpp"

namespace Rtx
{
    class ContentSource;

    /// Reads one cell — its ground and its paged references — into a `PreparedCell`, on whichever
    /// thread owns this: what `Terrain::ObjectPaging::createChunk` reads through
    /// `Terrain::ObjectStorage`, and none of what it merges. A model is read once and lent to
    /// every cell that names it, and so is an image; a lend is a cell's and not the frame's,
    /// because the frame cannot know what this has lent since it last looked, and `CellHolds`
    /// counts what the frame holds beside it. Not thread-safe, and one instance a thread.
    class CellReader
    {
    public:
        /// @param mask which nodes the walk of a template may descend into — the frame walk's own
        ///        traversal mask, so the two reach the same drawables.
        CellReader(const Terrain::ObjectStorage& storage, Terrain::Storage& ground, ContentSource& content,
            ESM::RefId worldspace, osg::Node::NodeMask mask);

        /// Reads the cell at `cell`: its ground, its `LIGH` references, and — where `statics` —
        /// every reference that pages and names a model with something to trace, as one
        /// `PreparedRef` each. The cell is lent, and `giveBack` is where it returns; every model and
        /// every ground texture it names is lent to it as well, one hold each, and those come back
        /// on their own.
        PreparedCell& read(const osg::Vec2i& cell, bool statics);

        /// Takes a cell back, once the frame has copied what it wanted of it.
        void giveBack(PreparedCell& cell);

        /// Takes back one cell's hold on a model. The model goes where it was the last, and its
        /// images with it where nothing else names them.
        void giveBack(PreparedModel& model);

        /// Takes back one cell's hold on an image its ground named. The image goes where it was the
        /// last.
        void giveBack(PreparedTexture& texture);

    private:
        /// Reads the cell into `prepared`, which `read` has taken and lends after: the whole of a
        /// read but the pool's part.
        void fill(PreparedCell& prepared, const osg::Vec2i& cell, bool statics);

        /// The model at `path`, read whole where this holds none under that path. Null where
        /// nothing stands for the path. Refused, with no part and `PreparedModel::mRefused` saying
        /// why, where the template describes a mesh this renderer cannot take.
        PreparedModel* readModel(VFS::Path::NormalizedView path);

        /// The reading of a layer's texture at `path`, made where this holds none under that path,
        /// and one more holder counted on it: its diffuse, and its normal map where it has one.
        PreparedTexture& readTexture(const osg::ref_ptr<const osg::Image>& image, const VFS::Path::Normalized& path);

        const Terrain::ObjectStorage& mStorage;
        ContentSource& mContent;
        ESM::RefId mWorldspace;
        osg::Node::NodeMask mMask;

        GroundReader mGround;
        TemplateWalk mWalk;

        // Refilled per cell, per model and per image.
        std::vector<Terrain::PagedCellRef> mRefScratch;

        /// A byte per reference of `mRefScratch`, set where the reference is a lamp.
        std::vector<std::uint8_t> mIsLampScratch;

        Spares<PreparedCell> mCells;
        Spares<PreparedModel> mModels;
        Spares<PreparedTexture> mTextures;

        /// What the two tables are ordered by: the path each is filed under.
        struct PathOf
        {
            std::string_view operator()(const PreparedModel* held) const { return held->mPath; }
            std::string_view operator()(const PreparedTexture* held) const { return held->mPath.value(); }
        };

        /// Every model and every ground texture lent, sorted by path — searched rather than keyed,
        /// because a lookup then costs no node and no string. By path and not by image, because a
        /// texture that does not read has no image and still stands, as the stand-in.
        boost::container::flat_set<PreparedModel*, KeyedLess<std::string_view, PathOf>, std::vector<PreparedModel*>>
            mModelsByPath;
        boost::container::flat_set<PreparedTexture*, KeyedLess<std::string_view, PathOf>, std::vector<PreparedTexture*>>
            mTexturesByPath;
    };
}
