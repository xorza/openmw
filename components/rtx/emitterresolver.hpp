#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include <osg/Drawable>
#include <osg/Matrixf>
#include <osg/Vec3f>

#include "mirroridentity.hpp"
#include "runs.hpp"
#include "scenedesc.hpp"
#include "sprite.hpp"
#include "surface.hpp"
#include "walk.hpp"

namespace osg
{
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
        /// What one particle system draws with, read off its state-set chain once and kept: its
        /// sprite texture in `mIndex`, the bake of that texture's alpha its sprites are lit by, how
        /// they composite, and the image itself. Read again only where a link of the chain
        /// animates, because nothing else can change what a system draws with — and describing
        /// a chain is a walk of its state sets, which hundreds of emitters a frame paid for
        /// nothing.
        struct HeldSprite : Known
        {
            Index mLighting = sNoIndex;

            /// How the system's sprites composite: one that adds is light and must not be lit.
            BlendKind mBlend = BlendKind::Over;

            /// What the sprites' colour and alpha are read off, which is the material's vertex
            /// mode: under `Tint` the particle's own colour and its two alpha ramps, as
            /// `osgParticle` hands them to the vertex; under anything else the material's diffuse
            /// and its opacity, with the particle's ignored — which is what the rasterizer's
            /// `getDiffuseColor` reads under `None`, and what the mist in every ancestral tomb is
            /// authored as: a material at half opacity over a particle that says one.
            VertexColour mVertexColour = VertexColour::None;
            osg::Vec3f mDiffuseColour{ 1.0f, 1.0f, 1.0f };
            float mOpacity = 1.0f;

            /// The image the sprites are drawn with, or null for a system nothing described a
            /// sprite for, which draws nothing and is counted. What a rewrite is told apart by,
            /// and what the census names once per emitter.
            const osg::Image* mSprite = nullptr;
        };

        /// An emitter the walk met, waiting for the walk to finish before its particles are read.
        struct Pending
        {
            const osgParticle::ParticleSystem* mParticles;
            osg::Matrixf mPlace;

            /// The map's own entry, which holds its place until `retire`, after every flush.
            const HeldSprite* mHeld;

            /// Whether its sprites fall from the sky, which is the walk's word and not the system's.
            bool mFalls;
        };

        /// Reads what a system draws with off its chain into `held`, taking the scene's slots for
        /// an image it did not hold and giving back the ones for an image it no longer wears.
        void describeSprite(HeldSprite& held, std::span<const Shading> shading);

        /// Gives back the slots `held` took, where it took any.
        void releaseSprite(const HeldSprite& held);

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

        /// Noted now and read when the walk is over. Whether a system has been integrated this
        /// frame depends on where its `ParticleSystemUpdater` sits among its siblings — above it in
        /// everything `NifOsg` builds, but that is the content's promise and not this walk's.
        /// Reading after the walk has settled is what makes the question stop existing.
        std::vector<Pending> mPending;
    };
}
