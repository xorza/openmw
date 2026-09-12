#pragma once

#include <span>
#include <vector>

#include <osg/Matrixf>

#include "mirroridentity.hpp"
#include "runs.hpp"
#include "scenedesc.hpp"
#include "walk.hpp"

namespace osg
{
    class Drawable;
    class Image;
}

namespace osgParticle
{
    class ParticleSystem;
}

namespace Rtx
{
    struct Shading;

    /// Turns the particle systems a walk met into the scene's sprites: a run of discs the trace
    /// composites against the primary ray, resolved apart from the meshes because a particle
    /// system carries no triangles. The engine's own simulation, read where it stands, because a
    /// second implementation of `NiParticleSystemController` is free to disagree with the game's.
    class EmitterResolver
    {
    public:
        /// @param pass the walk in progress: its sweep stamp and its counts, read at every call.
        ///        Borrowed, so that the mirror and everything resolving into it cannot come to hold
        ///        two answers.
        EmitterResolver(SceneDesc& scene, const MirrorPass& pass)
            : mScene(scene)
            , mPass(pass)
        {
        }

        /// Notes one system the walk met, to be read when the walk is over.
        void add(
            const osgParticle::ParticleSystem& particles, std::span<const Shading> shading, const osg::Matrixf& place);

        /// Reads every system noted, now that everything in the graph has been stepped.
        void flush();

        /// Lets go of the textures of every system this epoch did not meet.
        void retire();

        /// Reserves the identity map once, so no frame rehashes it. `SceneExtractor` states the
        /// budget.
        void reserve(std::size_t emitters) { mHeld.reserve(emitters); }

    private:
        /// An emitter the walk met, waiting for the walk to finish before its particles are read.
        struct Pending
        {
            const osgParticle::ParticleSystem* mParticles;
            osg::Matrixf mPlace;
            Index mTexture;
            Index mLighting;
            bool mLight;

            /// Kept only to name the texture's format in the stats, which is read once per emitter.
            const osg::Image* mSprite;
        };

        /// What one particle system draws with: its sprite texture in `mIndex`, and the bake of that
        /// texture's alpha its sprites are lit by.
        struct HeldSprite : Known
        {
            Index mLighting = sNoIndex;
        };

        /// Reads one noted system into the scene.
        void placeSprites(const Pending& pending);

        SceneDesc& mScene;
        const MirrorPass& mPass;

        /// Which textures each particle system draws with. This entry is the reference: a sprite's
        /// texture hangs off no material, so the scene holds it from first meeting until the sweep
        /// loses the emitter.
        Identity<const osg::Drawable, HeldSprite> mHeld{ mPass };

        /// Refilled per emitter and never freed: a cell's plumes are hundreds of discs apiece.
        std::vector<Sprite> mSpriteScratch;

        /// **Noted now and read when the walk is over.** Whether a system has been integrated this
        /// frame depends on where its `ParticleSystemUpdater` sits among its siblings — above it in
        /// everything `NifOsg` builds, but that is the content's promise and not this walk's.
        /// Reading after the walk has settled is what makes the question stop existing.
        std::vector<Pending> mPending;
    };
}
