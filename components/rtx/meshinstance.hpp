#pragma once

#include <osg/Matrixf>

#include "index.hpp"

namespace Rtx
{
    /// One mesh placed in the world: a row of the top-level acceleration structure.
    ///
    /// Not `Instance`, which in this namespace is the `VkInstance` a device comes from.
    struct MeshInstance
    {
        /// Object space to world space.
        osg::Matrixf mTransform;

        Index mMesh = sNoIndex;
        Index mMaterial = sNoIndex;

        /// How much of this placement is there — the fade the game is applying to one actor.
        ///
        /// **On the placement and never on the material, because a material is shared.**
        /// `SceneUtil::CopyOp` does not deep-copy state sets, so every actor built from one body
        /// part reads one material, and only the one walking out of `actors processing range` is
        /// fading. `MWRender::TransparencyUpdater` writes the number on a state set above the whole
        /// actor for that same reason, and this is where it lands — with Invisibility and Chameleon,
        /// which ride the same pair of uniforms.
        ///
        /// One for everything the game is not hiding, which is nearly everything.
        float mOpacity = 1.0f;

        /// Whether this is the player's own arms in first person, which only the eye's ray may
        /// meet. `Shaders::MASK_FIRST_PERSON` says why.
        bool mFirstPerson = false;

        /// Whether this slot holds anything. A dropped placement leaves its slot behind rather than
        /// closing the gap, because the slot index is what a hit reads back.
        bool isPlaced() const { return mMesh != sNoIndex; }
    };
}
