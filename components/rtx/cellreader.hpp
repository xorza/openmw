#pragma once

#include <string_view>
#include <vector>

#include <osg/Image>
#include <osg/Node>
#include <osg/Vec2i>

#include <components/esm/refid.hpp>
#include <components/terrain/objectstorage.hpp>
#include <components/terrain/storage.hpp>
#include <components/vfs/pathutil.hpp>

#include "alphaimage.hpp"
#include "groundreader.hpp"
#include "meshreader.hpp"
#include "nodekind.hpp"
#include "prepared.hpp"
#include "scratch.hpp"
#include "slots.hpp"
#include "templatewalk.hpp"
#include "texturedata.hpp"

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

        /// Reads the cell at `cell`: its ground, and — where `statics` — every reference that pages
        /// and names a model with something to trace, as one `PreparedRef` each. The cell is lent,
        /// and `giveBack` is where it returns; every model and every ground texture it names is
        /// lent to it as well, one hold each, and those come back on their own.
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
        /// The model at `path`, read whole where this holds none under that path. Null where nothing
        /// stands for the path.
        PreparedModel* readModel(VFS::Path::NormalizedView path);

        /// The image described, read where this holds no description of it, and one more holder
        /// counted on it. Null where the image names no file, which is an image no slot is ever
        /// made for.
        PreparedTexture* readTexture(const osg::Image& image);

        const Terrain::ObjectStorage& mStorage;
        ContentSource& mContent;
        ESM::RefId mWorldspace;
        osg::Node::NodeMask mMask;

        GroundReader mGround;
        TemplateWalk mWalk;
        MeshReader mMeshes;

        /// This thread's own classifier: `NodeKinds` is written on a miss.
        NodeKinds mKinds;
        AlphaScratch mAlpha;

        // Refilled per cell, per model and per image.
        std::vector<Terrain::PagedCellRef> mRefScratch;
        std::vector<MipLevel> mLevelScratch;

        Spares<PreparedCell> mCells;
        Spares<PreparedModel> mModels;
        Spares<PreparedTexture> mTextures;

        /// What the two tables are ordered by, stated once each.
        struct PathOf
        {
            std::string_view operator()(const PreparedModel* held) const { return held->mPath; }
        };

        struct ImageOf
        {
            const osg::Image* operator()(const PreparedTexture* held) const { return held->mImage.get(); }
        };

        /// Every model lent, sorted by path, and every image lent, sorted by address — searched
        /// rather than keyed, because a lookup then costs no node and no string.
        SortedRows<PreparedModel*, std::string_view, PathOf> mByPath;
        SortedRows<PreparedTexture*, const osg::Image*, ImageOf> mByImage;
    };
}
