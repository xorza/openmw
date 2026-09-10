#include "groundreader.hpp"

#include <cassert>

#include <osg/Array>
#include <osg/GL>
#include <osg/Image>
#include <osg/PrimitiveSet>
#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <components/esm/util.hpp>

#include "contentsource.hpp"

namespace Rtx
{
    namespace
    {
        /// The weights of one blend map, as floats in row order, appended to `weights`.
        ///
        /// `ESMTerrain` builds these as one byte per texel in `GL_ALPHA`, which is a kilobyte for a
        /// cell; widening them costs a few kilobytes a cell and saves requiring 8-bit storage of the
        /// device for the sake of it.
        ///
        /// **That one format is read along the row, and everything else asks `getColor`.**
        /// `getColor` decides on the pixel format and the data type per texel and builds a `Vec4` to
        /// hand back one component of it. A blend map in any other format is a mod's or a test's,
        /// and the slow path is both what serves it and what the fast path is checked against.
        void readMask(const osg::Image& image, std::vector<float>& weights)
        {
            if (image.getPixelFormat() == GL_ALPHA && image.getDataType() == GL_UNSIGNED_BYTE)
            {
                // **The reciprocal and not a divide, because that is `getColor`'s own arithmetic.**
                // The two disagree in the last place for 126 of the 256 byte values, and a weight is
                // what a cell's ground is blended by and what its composite is baked from — so a
                // divide here would move the picture by a bit and the scene digests with it.
                constexpr float perByte = 1.0f / 255.0f;

                for (int row = 0; row < image.t(); ++row)
                {
                    const unsigned char* along = image.data(0, row);
                    for (int column = 0; column < image.s(); ++column)
                        weights.push_back(along[column] * perByte);
                }
                return;
            }

            for (int row = 0; row < image.t(); ++row)
                for (int column = 0; column < image.s(); ++column)
                    weights.push_back(image.getColor(column, row).a());
        }
    }

    GroundReader::GroundReader(Terrain::Storage& storage, ContentSource& content, const ESM::RefId worldspace)
        : mStorage(storage)
        , mContent(content)
        , mWorldspace(worldspace)
        , mCellSize(storage.getCellWorldSize(worldspace))
        , mVerts(static_cast<std::size_t>(storage.getCellVertices(worldspace)))
        , mTileCount(storage.getTextureTileCount(1.0f, worldspace))
        , mEsm4(ESM::isEsm4Ext(worldspace))
    {
        Terrain::BufferCache buffers;
        copyGrid(buffers, static_cast<unsigned int>(mVerts), mGridIndices, mGridCorners);
        copyGrid(buffers, 2, mQuadIndices, mQuadCorners);
    }

    osg::Vec4f GroundReader::diffuseTransform(const int tileCount)
    {
        const auto tiles = static_cast<float>(tileCount);
        return osg::Vec4f(tiles, tiles, 0.0f, 0.0f);
    }

    osg::Vec4f GroundReader::maskTransform(const int tileCount)
    {
        const auto tiles = static_cast<float>(tileCount);
        const float scale = tiles / (tiles + 1.0f);
        const float nudge = 1.0f / tiles / 4.0f;

        // `T(0.5) · S(scale) · T(-0.5) · T(nudge, -nudge)` applied to a row vector, which is the
        // order `preMult` composes them in there: the nudge first, then the scale about the centre.
        return osg::Vec4f(scale, scale, (nudge - 0.5f) * scale + 0.5f, (-nudge - 0.5f) * scale + 0.5f);
    }

    void GroundReader::copyGrid(Terrain::BufferCache& buffers, const unsigned int verts,
        std::vector<std::uint32_t>& indices, std::vector<osg::Vec2f>& corners)
    {
        // **The rasterizer's own triangles and corners, with no stitching flags**: a vertex is
        // `col * verts + row`, which is the order `fillVertexBuffers` writes them in, and the
        // diamond alternates which diagonal a quad is split along — so a slope is cut the way the
        // game cuts it, from the one place that says how.
        const osg::ref_ptr<osg::DrawElements> triangles = buffers.getIndexBuffer(verts, 0);
        indices.clear();
        indices.reserve(triangles->getNumIndices());
        for (unsigned int at = 0; at < triangles->getNumIndices(); ++at)
            indices.push_back(triangles->index(at));

        const osg::ref_ptr<osg::Vec2Array> uvs = buffers.getUVBuffer(verts);
        corners.assign(uvs->begin(), uvs->end());
    }

    void GroundReader::read(const osg::Vec2i& cell, PreparedGround& into)
    {
        const osg::Vec2f centre(static_cast<float>(cell.x()) + 0.5f, static_cast<float>(cell.y()) + 0.5f);
        into.mOrigin = osg::Vec3f(centre.x() * mCellSize, centre.y() * mCellSize, 0.0f);

        const ESM::ExteriorCellLocation location(cell.x(), cell.y(), mWorldspace);
        if (mStorage.hasData(location))
        {
            mStorage.fillVertexBuffers(0, 1.0f, centre, mWorldspace, *mPositions, *mNormals, *mColours);
            assert(mPositions->size() == mVerts * mVerts);

            into.mPositions.assign(mPositions->begin(), mPositions->end());
            into.mNormals.assign(mNormals->begin(), mNormals->end());
            into.mTexCoords = mGridCorners;
            into.mIndices = mGridIndices;
        }
        else
        {
            // **A cell with no land record is a plane at the default height**, which is what the
            // storage answers for any point in it — and a plane is four corners, not a grid of
            // them. Every cell of the reach has a bed under its sea this way, where the quad tree
            // stands nothing past the active grid and the grid's own fallback stands the whole
            // grid. An ESM4 world has no default height: `fillVertexBuffers` zeroes the positions
            // there, which is no ground at all.
            if (mEsm4)
                return;

            const float height = mStorage.getHeightAt(into.mOrigin, mWorldspace);
            for (const osg::Vec2f& corner : mQuadCorners)
            {
                into.mPositions.emplace_back((corner.x() - 0.5f) * mCellSize, (corner.y() - 0.5f) * mCellSize, height);
                into.mNormals.emplace_back(0.0f, 0.0f, 1.0f);
            }

            into.mTexCoords = mQuadCorners;
            into.mIndices = mQuadIndices;
        }

        into.mStands = true;

        // **The layers as the chunk manager reads them**, one blend map per ground type where there
        // is more than one, and none where a single type covers the cell.
        mBlendmaps.clear();
        mLayerInfos.clear();
        mStorage.getBlendmaps(1.0f, centre, mBlendmaps, mLayerInfos, mWorldspace);
        assert(mBlendmaps.empty() || mBlendmaps.size() == mLayerInfos.size());

        for (std::size_t index = 0; index < mLayerInfos.size(); ++index)
        {
            osg::ref_ptr<const osg::Image> image = mContent.getImage(mLayerInfos[index].mDiffuseMap);
            if (image == nullptr)
                continue;

            PreparedLayer layer;
            layer.mImage = std::move(image);
            layer.mDiffuseTransform = diffuseTransform(mTileCount);

            if (!mBlendmaps.empty() && mBlendmaps[index] != nullptr)
            {
                const osg::Image& mask = *mBlendmaps[index];
                layer.mFirstWeight = static_cast<std::uint32_t>(into.mWeights.size());
                readMask(mask, into.mWeights);
                layer.mWeightCount = static_cast<std::uint32_t>(into.mWeights.size()) - layer.mFirstWeight;
                layer.mMaskWidth = static_cast<std::uint16_t>(mask.s());
                layer.mMaskHeight = static_cast<std::uint16_t>(mask.t());

                // An ESM4 blend map is sampled as it stands: `createPasses` attaches no matrix
                // for one.
                if (!mEsm4)
                    layer.mMaskTransform = maskTransform(mTileCount);
            }

            into.mLayers.push_back(std::move(layer));
        }
    }
}
