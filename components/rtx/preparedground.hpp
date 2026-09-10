#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <osg/Image>
#include <osg/Vec2f>
#include <osg/Vec3f>
#include <osg/Vec4f>
#include <osg/ref_ptr>

#include "run.hpp"

namespace Rtx
{
    struct PreparedTexture;

    /// One ground texture a cell's land names, and the weights that place it.
    struct PreparedLayer
    {
        /// The tiling ground texture, opened on the thread. What its reading is looked up by.
        osg::ref_ptr<const osg::Image> mImage;

        /// The reader's description of it, lent for as long as the cell is held.
        PreparedTexture* mTexture = nullptr;

        /// Into `PreparedGround::mWeights`. An empty run is a layer covering the whole cell, which
        /// is what a cell of one ground type is given.
        ///
        /// **A run, so the reader and the scene say a mask the same way.** The `MaterialLayer` this
        /// becomes already holds one.
        Run mWeights;
        std::uint16_t mMaskWidth = 0;
        std::uint16_t mMaskHeight = 0;

        /// Cell texture coordinates to this layer's, as `uv * xy + zw`. `GroundReader` says where
        /// the two come from.
        osg::Vec4f mDiffuseTransform{ 1.0f, 1.0f, 0.0f, 0.0f };
        osg::Vec4f mMaskTransform{ 1.0f, 1.0f, 0.0f, 0.0f };
    };

    /// One cell's ground, read off the land records on a thread that is not the frame's.
    ///
    /// **A cell and not a chunk.** The quad tree cut the ground by the eye's distance and stitched
    /// the cuts; a cell's heights are the content's own 65 × 65 grid, whose edge rows its neighbours
    /// share, so two cells meet vertex for vertex whatever the eye does and nothing is ever rebuilt
    /// for a neighbour.
    ///
    /// Part of a `PreparedCell`, and lent with it.
    struct PreparedGround
    {
        /// False where the land names no ground at all, which stands nothing.
        bool mStands = false;

        /// The cell's centre in world units, which the positions are relative to.
        osg::Vec3f mOrigin;

        std::vector<osg::Vec3f> mPositions;
        std::vector<osg::Vec3f> mNormals;

        /// The land's own per-vertex colour, in linear light — `VCLR`, which the designers painted
        /// over two thirds of Morrowind's exterior vertices and which tints the ground under
        /// everything the layers place on it.
        ///
        /// White for a cell with no land record, and white wherever the record left 255.
        std::vector<osg::Vec3f> mColours;

        /// The reader's own, shared by every cell of one shape: a grid's corners and its
        /// triangulation are the same for every cell.
        std::span<const osg::Vec2f> mTexCoords;
        std::span<const std::uint32_t> mIndices;

        std::vector<PreparedLayer> mLayers;

        /// Every layer's weights end to end, row by row.
        std::vector<float> mWeights;

        /// Makes room for the next cell, keeping what the buffers grew.
        void reuse()
        {
            mStands = false;
            mPositions.clear();
            mNormals.clear();
            mColours.clear();
            mTexCoords = {};
            mIndices = {};
            mLayers.clear();
            mWeights.clear();
        }
    };
}
