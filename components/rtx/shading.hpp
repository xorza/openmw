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

        /// Whether this state set or any above it is a controller's, resolved as the chain is built
        /// the way `mFade` is, rather than counted beside it where the two could part.
        ///
        /// **What stands under a controller is animated by it.** `SceneUtil::addEnchantedGlow` hangs
        /// its sheet on an instance's root, above the shape the sheet is read into, and that shape's
        /// own state set is shared by every instance of the model — so a material keyed on it stands
        /// for the enchanted sword and the plain one beside it at once. `MaterialResolver::animate`
        /// is what this is asked for.
        bool mAnimatedThrough = false;
    };

    /// Whether a controller's state set stands anywhere on `shading` — `Shading::mAnimatedThrough`
    /// at the near end, which is that question resolved as the chain was built rather than asked of
    /// each link. What a material keyed on the near end has to know, and what says a sprite's
    /// reading is this frame's rather than the one held.
    inline bool animatedThrough(std::span<const Shading> shading)
    {
        return !shading.empty() && shading.back().mAnimatedThrough;
    }

    /// What the content said this surface is, folded out of the chain of state sets in force at it,
    /// root first and nearest last, which is how OpenGL resolves the same chain — a parent's
    /// `OVERRIDE` included, through one `SurfaceLocks` carried down it. False where no state set
    /// on the chain carried a material or a texture — the sky, the water, a debug line — and
    /// `material` is then the defaults.
    bool describeSurface(std::span<const Shading> shading, SurfaceDescription& material);

    /// How much of an actor there is under `stateSet`, from the pair of uniforms
    /// `MWRender::TransparencyUpdater` writes, or `inherited` where it carries neither. Both off
    /// one state set, because `NifOsg::AlphaController` writes `alpha` alone and a walk that took
    /// any `alpha` would fade an animated surface twice.
    float fadeThrough(const osg::StateSet& stateSet, float inherited);
}
