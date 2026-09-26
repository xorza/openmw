#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <osg/Array>
#include <osg/Image>
#include <osg/Vec2f>
#include <osg/Vec2i>
#include <osg/Vec4f>
#include <osg/ref_ptr>

#include <components/esm/refid.hpp>
#include <components/terrain/buffercache.hpp>
#include <components/terrain/defs.hpp>
#include <components/terrain/storage.hpp>
#include <components/vfs/pathutil.hpp>

#include "prepared.hpp"

namespace Rtx
{
    class ContentSource;

    /// Reads one cell's ground off the land records into a `PreparedGround`, on whichever thread
    /// owns this: what `Terrain::ChunkManager::createChunk` reads from `Terrain::Storage` and
    /// `Terrain::BufferCache` for a chunk one cell wide at full detail, and none of the passes
    /// and cameras it builds around it. Not thread-safe, and one instance a thread.
    class GroundReader
    {
    public:
        GroundReader(Terrain::Storage& storage, ContentSource& content, ESM::RefId worldspace);

        /// Reads the ground of `cell` into `into`, which has been through `reuse`. A layer whose
        /// texture cannot be opened keeps its place and its path, for the texture table to stand
        /// in and refuse: left out, the ground under it would show another layer with nothing said.
        /// The layers' textures are `getLayerFiles`', for the caller to hold them by.
        void read(const osg::Vec2i& cell, PreparedGround& into);

        /// What one layer's land names and what opening it found: the diffuse, and the normal map
        /// the storage found beside it, with an empty path where there is none and a null image
        /// where a file would not read.
        struct LayerFiles
        {
            osg::ref_ptr<const osg::Image> mImage;
            VFS::Path::Normalized mPath;
            osg::ref_ptr<const osg::Image> mNormalImage;
            VFS::Path::Normalized mNormalPath;
        };

        /// The last `read`'s, one a layer in the layers' order, until the next `read`. Here and not
        /// on the layer, because a layer holds the reader's description of each texture, which
        /// holds the same pair for as long as any cell names it.
        std::span<const LayerFiles> getLayerFiles() const { return mFiles; }

        /// Cell texture coordinates to a layer's diffuse texture, which tiles `tileCount` times
        /// across the cell: what `LayerTexMat` in `components/terrain/material.cpp` attaches.
        static osg::Vec4f diffuseTransform(int tileCount);

        /// Cell texture coordinates to a layer's blend map, for a map of `tileCount` tiles a side
        /// doubled to match the original game's: what `BlendmapTexMat` attaches, as `uv * xy + zw`.
        /// Derived, because the class that states it is private to `material.cpp`, and a test
        /// asserts the numbers so a change there is a failure here rather than a drift.
        static osg::Vec4f maskTransform(int tileCount);

    private:
        /// Copies the game's own index and corner buffers for a grid `verts` a side, so that a cell
        /// spans a plain array rather than an `osg::DrawElements` of one of two widths.
        static void copyGrid(Terrain::BufferCache& buffers, unsigned int verts, std::vector<std::uint32_t>& indices,
            std::vector<osg::Vec2f>& corners);

        Terrain::Storage& mStorage;
        ContentSource& mContent;
        ESM::RefId mWorldspace;

        float mCellSize = 0.0f;
        std::size_t mVerts = 0;
        int mTileCount = 0;
        bool mEsm4 = false;

        /// A cell's grid, and the quad a cell with no land record stands as. Built once: every cell
        /// of one shape shares them, and a prepared cell spans them for as long as this stands.
        std::vector<std::uint32_t> mGridIndices;
        std::vector<osg::Vec2f> mGridCorners;
        std::vector<std::uint32_t> mQuadIndices;
        std::vector<osg::Vec2f> mQuadCorners;

        // What the storage fills, refilled per cell.
        osg::ref_ptr<osg::Vec3Array> mPositions = new osg::Vec3Array;
        osg::ref_ptr<osg::Vec3Array> mNormals = new osg::Vec3Array;

        /// The land's vertex colours as the storage fills them, display-encoded and one byte a
        /// channel. Decoded into `PreparedGround::mColours` per cell.
        osg::ref_ptr<osg::Vec4ubArray> mColours = new osg::Vec4ubArray;
        Terrain::Storage::ImageVector mBlendmaps;
        std::vector<Terrain::LayerInfo> mLayerInfos;
        std::vector<LayerFiles> mFiles;
    };
}
