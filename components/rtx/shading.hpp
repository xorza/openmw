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
    /// One state set in the chain that shades a drawable, nearest it last.
    ///
    /// **Not simply a node's own state set.** OpenMW animates shading by handing a
    /// `SceneUtil::StateSetUpdater` a state set that belongs to the traversal rather than to the
    /// graph — that is how a fire flips through its frames and how lava scrolls — so the state set
    /// in force at a drawable is something a walk builds and not something a node holds.
    struct Shading
    {
        const osg::StateSet* mStateSet = nullptr;

        /// How much of an actor there is at this point of the chain: the pair of uniforms the game
        /// fades one with, read off the nearest state set that carries them, or the value the chain
        /// already had where this one does not.
        ///
        /// **Resolved as the chain is built and not per drawable.** Every drawable used to walk its
        /// chain asking each state set for two uniforms by a `std::string` made on the spot; a
        /// state set is asked once now, when it is pushed, and a drawable reads the answer.
        float mFade = 1.0f;

        /// Whether a controller rewrote this since the last frame, so the material read from it is
        /// not the material it will be next frame. What tells `resolveMaterial` to read a known
        /// state set again instead of handing back the slot it already has.
        bool mAnimated = false;
    };

    /// What the content said this surface is, taken from the nearest ancestor that said it.
    ///
    /// **Nearest wins, which is what a NIF property does.** `NifOsg` stamps a complete material on
    /// the state set it resolves each shape against, so the first one found walking back up is
    /// already the whole answer; an ancestor's is what a shape that carries no state set of its own
    /// inherits.
    const Surface::Material* findDescription(std::span<const Shading> shading);

    /// Whether the nearest pass on the chain adds to the frame rather than covering it.
    ///
    /// **The nearest state set that has a blend function, not simply the nearest one.** A particle
    /// system carries a state set of its own that sets neither blending nor texture — `NifOsg` puts
    /// both on the transform above it — so asking the drawable's own would answer "covers" for every
    /// flame in the game.
    ///
    /// The split is the whole of what tells a flame from a puff of smoke, and 474 of the game's 678
    /// emitters are on the adding side. One that blends over is an albedo and has to be lit; one
    /// that adds *is* light and must not be.
    bool addsLight(std::span<const Shading> shading);

    /// How much of an actor there is under `stateSet`, from the two uniforms the game fades one
    /// with — or `inherited`, where it carries neither.
    ///
    /// **Both off the same state set, which is what tells them from a model's own animation.**
    /// `MWRender::TransparencyUpdater` writes `alpha` and `actorFade` as a pair on a state set above
    /// the whole actor, and the three things that ride them are the distance fade over the last
    /// tenth of `actors processing range`, Invisibility and Chameleon. `NifOsg::AlphaController`
    /// writes `alpha` on its own and writes the same number into the surface description as well —
    /// so a walk that took any `alpha` it met would fade an animated surface twice.
    ///
    /// The product is what `objects.frag` reaches for the same surface: `diffuseColor.a * alpha *
    /// actorFade`, of which the first factor is already in the material. Nearest wins, exactly as a
    /// rasterizing cull would resolve the uniform, which is what inheriting down the chain comes to;
    /// the scene root carries a pair of ones, so a chain that reaches the bottom answers the same as
    /// no chain.
    float fadeThrough(const osg::StateSet& stateSet, float inherited);
}
