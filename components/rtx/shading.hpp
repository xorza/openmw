#pragma once

#include <span>

namespace osg
{
    class StateSet;
}

namespace Rtx
{
    struct SurfaceDescription;

    /// One state set in the chain that shades a drawable, nearest it last. Not simply a node's own,
    /// because OpenMW animates shading with a `SceneUtil::StateSetUpdater`'s state set that belongs
    /// to the traversal.
    struct Shading
    {
        const osg::StateSet* mStateSet = nullptr;

        /// How much of an actor there is at this point of the chain, resolved as the chain is built
        /// rather than per drawable, which would ask each state set for two uniforms by a string
        /// made on the spot.
        float mFade = 1.0f;

        /// Whether a controller rewrote this since the last frame, so `MaterialResolver::resolve`
        /// reads a known state set again instead of handing back the slot it already has.
        bool mAnimated = false;
    };

    /// What the content said this surface is, folded out of the chain of state sets in force at it,
    /// root first and nearest last, which is how OpenGL resolves the same chain. False where no
    /// state set on the chain carried a material or a texture — the sky, the water, a debug line —
    /// and `material` is then the defaults.
    bool describeSurface(std::span<const Shading> shading, SurfaceDescription& material);

    /// Whether the nearest state set with a blend function adds to the frame rather than covering
    /// it — `NifOsg` puts a particle system's on the transform above it. What tells a flame from a
    /// puff of smoke: one that adds is light and must not be lit.
    bool addsLight(std::span<const Shading> shading);

    /// How much of an actor there is under `stateSet`, from the pair of uniforms
    /// `MWRender::TransparencyUpdater` writes, or `inherited` where it carries neither. Both off
    /// one state set, because `NifOsg::AlphaController` writes `alpha` alone and a walk that took
    /// any `alpha` would fade an animated surface twice.
    float fadeThrough(const osg::StateSet& stateSet, float inherited);
}
