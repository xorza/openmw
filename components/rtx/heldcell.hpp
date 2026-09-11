#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <osg/Matrixf>
#include <osg/Vec2i>
#include <osg/Vec3f>

#include <components/esm3/refnum.hpp>

#include "index.hpp"

namespace Rtx
{
    struct PreparedModel;
    struct PreparedTexture;

    /// One placement the ring may stand: a part of a model at a reference.
    struct Placement
    {
        Index mMesh = sNoIndex;
        Index mMaterial = sNoIndex;
        osg::Matrixf mTransform;
        float mRadius = 0.0f;
        ESM::RefNum mRefNum;

        /// The slot it stands in, or none while the size rule or the ring keeps it out.
        Index mSlot = sNoIndex;
    };

    /// A cell's ground as the frame holds it: its rows, where it stands, and what shades it.
    ///
    /// **Its own type, because the seven are empty together.** They were seven fields of the cell
    /// and `mGroundMesh == sNoIndex` stood for all of them — one rule a reader had to know rather
    /// than a shape that states it. `PreparedGround::mStands` says the same thing one step earlier.
    struct HeldGround
    {
        /// The rows the ring holds on the scene, which no drawable and no state set will ever name.
        Index mMesh = sNoIndex;
        Index mMaterial = sNoIndex;

        /// The slot it stands in, or none while the ring keeps it out.
        Index mSlot = sNoIndex;

        /// The cell's centre, which the mesh's own positions are relative to.
        osg::Vec3f mOrigin;

        /// How many layers the stack holds, which is whether a composite is worth asking for at all.
        std::uint32_t mLayers = 0;

        /// Whether the material row asks for a composite now, so a crossing of the active grid
        /// rewrites it once.
        bool mFlattened = false;

        /// The readings of its textures, held for as long as the cell is.
        std::vector<PreparedTexture*> mTextures;

        /// Empties it for the next cell, keeping the room the texture list grew.
        void reuse()
        {
            mMesh = sNoIndex;
            mMaterial = sNoIndex;
            mSlot = sNoIndex;
            mLayers = 0;
            mFlattened = false;
            mTextures.clear();
        }
    };

    /// A cell the frame has adopted. Its vectors are kept when it is dropped, so a cell that arrives
    /// later refills them rather than growing new ones — `Recycled`.
    struct HeldCell
    {
        osg::Vec2i mCell;

        /// Whether its references were read, so a cell read under the other setting is dropped and
        /// read again.
        bool mStatics = false;

        std::vector<Placement> mPlacements;
        std::vector<PreparedModel*> mModels;

        /// The ground, or nothing where the land names none.
        ///
        /// **The room its texture list grew is kept across a drop**, which is what the spare list
        /// takes a cell for — so what is emptied here is the optional's contents and never the
        /// optional itself.
        std::optional<HeldGround> mGround;
    };
}
