#pragma once

#include <span>

namespace osg
{
    class StateSet;
}

namespace Surface
{
    struct Material;
}

namespace Rtx
{
    /// One state set in the chain that shades a drawable, nearest it last. Not simply a node's own,
    /// because OpenMW animates shading with a `SceneUtil::StateSetUpdater`'s state set that belongs
    /// to the traversal.
    struct Shading
    {
        const osg::StateSet* mStateSet = nullptr;

        /// How much of an actor there is at this point of the chain: the pair of uniforms the game
        /// fades one with, or the value the chain already had. Resolved as the chain is built and
        /// not per drawable, which would ask each state set for two uniforms by a `std::string`
        /// made on the spot.
        float mFade = 1.0f;

        /// Whether a controller rewrote this since the last frame, so the material read from it is
        /// not the material it will be next frame. What tells `resolveMaterial` to read a known
        /// state set again instead of handing back the slot it already has.
        bool mAnimated = false;
    };

    /// What the content said this surface is, folded out of the chain of state sets in force at it,
    /// root first and nearest last, which is how OpenGL resolves the same chain.
    ///
    /// @return whether any state set on the chain carried a material or a texture. False is a
    ///         drawable that wears nothing — the sky, the water, a debug line — and `material` is
    ///         then the defaults.
    bool describeSurface(std::span<const Shading> shading, Surface::Material& material);

    /// Whether the nearest pass on the chain adds to the frame rather than covering it — the
    /// nearest state set that has a blend function, because `NifOsg` puts a particle system's on
    /// the transform above it. What tells a flame from a puff of smoke: one that adds *is* light
    /// and must not be lit, and 474 of the game's 678 emitters are on that side.
    bool addsLight(std::span<const Shading> shading);

    /// How much of an actor there is under `stateSet`, from the two uniforms the game fades one
    /// with — or `inherited`, where it carries neither. Both off the same state set, because
    /// `MWRender::TransparencyUpdater` writes `alpha` and `actorFade` as a pair, where
    /// `NifOsg::AlphaController` writes `alpha` alone and the same number into the surface
    /// description, so a walk that took any `alpha` would fade an animated surface twice. The
    /// product is what `objects.frag` reaches; nearest wins, as a cull would resolve the uniform.
    float fadeThrough(const osg::StateSet& stateSet, float inherited);
}
