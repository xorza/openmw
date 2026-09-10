#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Array>
#include <osg/GL>
#include <osg/Image>
#include <osg/Vec2f>
#include <osg/Vec2i>
#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <components/esm/exteriorcelllocation.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/loadland.hpp>
#include <components/misc/constants.hpp>
#include <components/terrain/defs.hpp>
#include <components/terrain/storage.hpp>
#include <components/vfs/pathutil.hpp>

namespace Rtx::Testing
{
    /// A land of a few cells with records and a default plane everywhere else, filled the way
    /// `ESMTerrain::Storage` fills a chunk one cell wide at full detail.
    ///
    /// **Heights a test can compute by hand**: a plane of eight units a column and sixteen a row
    /// over every cell with a record. Every such cell is two ground types, grass on its western
    /// half and rock on its eastern, on the 34 × 34 blend maps `getBlendmaps` builds for Morrowind.
    class FakeLand : public Terrain::Storage
    {
    public:
        static constexpr int sVerts = ESM::Land::LAND_SIZE;
        static constexpr int sTiles = ESM::Land::LAND_TEXTURE_SIZE;
        static constexpr int sMaskSide = 2 * (sTiles + 1);
        static constexpr float sCellSize = static_cast<float>(Constants::CellSizeInUnits);

        /// The cells with a land record.
        std::vector<osg::Vec2i> mWithData;

        static float heightAt(const int column, const int row) { return 8.0f * column + 16.0f * row; }

        /// Where the storage puts vertex (`column`, `row`) of a cell, relative to its centre.
        static osg::Vec3f positionAt(const int column, const int row)
        {
            const float side = static_cast<float>(sVerts - 1);
            return osg::Vec3f(
                (column / side - 0.5f) * sCellSize, (row / side - 0.5f) * sCellSize, heightAt(column, row));
        }

        /// The colour the storage puts on vertex (`column`, `row`), display-encoded as `VCLR` is.
        ///
        /// **Not white, because a white tint is the answer a renderer that dropped it gives.** The
        /// three channels differ from each other so that a test cannot pass on a decode that
        /// swapped them.
        static osg::Vec4ub colourAt(const int column, const int row)
        {
            const auto red = static_cast<unsigned char>(64 + column % 8);
            const auto green = static_cast<unsigned char>(128 + row % 8);
            return osg::Vec4ub(red, green, 255, 255);
        }

        bool hasData(const ESM::ExteriorCellLocation cell) override
        {
            return std::find(mWithData.begin(), mWithData.end(), osg::Vec2i(cell.mX, cell.mY)) != mWithData.end();
        }

        void getBounds(float& minX, float& maxX, float& minY, float& maxY, ESM::RefId) override
        {
            minX = -100.0f;
            maxX = 100.0f;
            minY = -100.0f;
            maxY = 100.0f;
        }

        bool getMinMaxHeights(
            float, const osg::Vec2f& centre, const ESM::RefId worldspace, float& min, float& max) override
        {
            min = heightAt(0, 0);
            max = heightAt(sVerts - 1, sVerts - 1);
            return hasData(cellOf(centre, worldspace));
        }

        void fillVertexBuffers(const int lod, const float size, const osg::Vec2f& centre, const ESM::RefId worldspace,
            osg::Vec3Array& positions, osg::Vec3Array& normals, osg::Vec4ubArray& colours) override
        {
            EXPECT_EQ(lod, 0) << "the ring reads every cell at full detail";
            EXPECT_EQ(size, 1.0f) << "and one cell at a time";
            EXPECT_TRUE(hasData(cellOf(centre, worldspace))) << "a cell with no record is not filled";

            positions.resize(static_cast<std::size_t>(sVerts) * sVerts);
            normals.resize(positions.size());
            colours.resize(positions.size());

            for (int column = 0; column < sVerts; ++column)
                for (int row = 0; row < sVerts; ++row)
                {
                    const std::size_t at = static_cast<std::size_t>(column) * sVerts + row;
                    positions[at] = positionAt(column, row);
                    normals[at] = osg::Vec3f(0.0f, 0.0f, 1.0f);
                    colours[at] = colourAt(column, row);
                }
        }

        void getBlendmaps(float, const osg::Vec2f& centre, ImageVector& blendmaps,
            std::vector<Terrain::LayerInfo>& layers, const ESM::RefId worldspace) override
        {
            if (!hasData(cellOf(centre, worldspace)))
            {
                layers.push_back(layerOf("textures/_land_default.dds"));
                return;
            }

            layers.push_back(layerOf("textures/grass.dds"));
            layers.push_back(layerOf("textures/rock.dds"));
            blendmaps.push_back(half(true));
            blendmaps.push_back(half(false));
        }

        float getHeightAt(const osg::Vec3f&, ESM::RefId) override { return ESM::Land::DEFAULT_HEIGHT; }
        float getCellWorldSize(ESM::RefId) override { return sCellSize; }
        int getCellVertices(ESM::RefId) override { return sVerts; }
        int getTextureTileCount(const float chunkSize, ESM::RefId) override
        {
            return static_cast<int>(sTiles * chunkSize);
        }

    private:
        static ESM::ExteriorCellLocation cellOf(const osg::Vec2f& centre, const ESM::RefId worldspace)
        {
            return ESM::ExteriorCellLocation(
                static_cast<int>(std::floor(centre.x())), static_cast<int>(std::floor(centre.y())), worldspace);
        }

        static Terrain::LayerInfo layerOf(const char* path)
        {
            return Terrain::LayerInfo{ VFS::Path::Normalized(path), VFS::Path::Normalized(), false, false };
        }

        /// A blend map covering the western or the eastern half of the cell, as `ESMTerrain`
        /// builds one: one byte a texel, rows of `sMaskSide`.
        static osg::ref_ptr<osg::Image> half(const bool western)
        {
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->allocateImage(sMaskSide, sMaskSide, 1, GL_ALPHA, GL_UNSIGNED_BYTE);
            std::memset(image->data(), 0, image->getTotalSizeInBytes());

            for (int row = 0; row < sMaskSide; ++row)
            {
                unsigned char* along = image->data(0, row);
                for (int column = 0; column < sMaskSide; ++column)
                    along[column] = (column < sMaskSide / 2) == western ? 255 : 0;
            }

            return image;
        }
    };

    /// Images by path, one object per path however often it is asked for — which is how a loader's
    /// cache answers, and what lets a reading made against one be found by the other.
    class ImagesByPath
    {
    public:
        osg::ref_ptr<const osg::Image> get(const VFS::Path::NormalizedView path)
        {
            const auto found = mImages.find(path.value());
            if (found != mImages.end())
                return found->second;

            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->setFileName(std::string(path.value()));
            image->allocateImage(4, 4, 1, GL_RGBA, GL_UNSIGNED_BYTE);
            std::fill_n(image->data(), image->getTotalSizeInBytes(), static_cast<unsigned char>(128));

            mImages.emplace(std::string(path.value()), image);
            return image;
        }

    private:
        std::map<std::string, osg::ref_ptr<osg::Image>, std::less<>> mImages;
    };
}
