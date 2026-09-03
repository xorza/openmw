#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <osg/Matrixf>

#include "index.hpp"
#include "mirroridentity.hpp"
#include "scenedesc.hpp"

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
    struct ExtractionStats;
    struct Shading;

    /// Turns the particle systems a walk met into the scene's sprites.
    ///
    /// **A particle system carries no triangles at all**, so nothing here reaches an acceleration
    /// structure: what leaves is a run of discs the trace composites against the primary ray. That
    /// is why the emitters are resolved on their own rather than beside the meshes — the two share
    /// the sweep and nothing else.
    ///
    /// **The engine's own simulation, read where it stands.** OpenMW runs `osgParticle` under the
    /// update traversal, so by the time the mirror walks the graph a flame is a list of positions,
    /// sizes and colours. Re-deriving that from the `NiParticleSystemController` would be a second
    /// implementation of the same content, free to disagree with the one the game is running.
    class EmitterResolver
    {
    public:
        /// @param epoch the mirror's sweep stamp, read at every call. Borrowed, so that the mirror
        ///        and everything resolving into it cannot come to hold two answers.
        EmitterResolver(SceneDesc& scene, const std::uint64_t& epoch)
            : mScene(scene)
            , mEpoch(epoch)
        {
        }

        /// Notes one system the walk met, to be read when the walk is over.
        void add(const osgParticle::ParticleSystem& particles, std::span<const Shading> shading,
            const osg::Matrixf& place, ExtractionStats& stats);

        /// Reads every system noted, now that everything in the graph has been stepped.
        void flush(ExtractionStats& stats);

        /// Lets go of the textures of every system this epoch did not meet.
        void retire();

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
        void placeSprites(const Pending& pending, ExtractionStats& stats);

        SceneDesc& mScene;
        const std::uint64_t& mEpoch;

        /// Which textures each particle system draws with.
        ///
        /// **This entry is the reference**, and not a note about one: a sprite's texture hangs off
        /// no material, so the scene is told to hold it when the emitter is first met and to let go
        /// when the sweep loses it. It saves a path hash per emitter per frame as well.
        Identity<const osg::Drawable, HeldSprite> mHeld;

        /// Refilled per emitter and never freed: a cell's plumes are hundreds of discs apiece.
        std::vector<Sprite> mSpriteScratch;

        /// **Noted now and read when the walk is over.** Whether a system has been integrated this
        /// frame depends on where its `ParticleSystemUpdater` sits among its siblings — above it in
        /// everything `NifOsg` builds, but that is the content's promise and not this walk's.
        /// Reading after the walk has settled is what makes the question stop existing.
        std::vector<Pending> mPending;
    };
}
