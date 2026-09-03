#include "emitterresolver.hpp"

#include <osgParticle/Particle>
#include <osgParticle/ParticleSystem>

#include <components/surface/material.hpp>
#include <components/vfs/pathutil.hpp>

#include "extractionstats.hpp"
#include "scenedesc.hpp"
#include "shading.hpp"
#include "spritelight.hpp"

namespace Rtx
{
    namespace
    {
        /// The uniform scale a placement carries, as the length of its first basis row.
        ///
        /// A sprite's size is in the particle system's own coordinates — `LOCAL_COORDINATES` is what
        /// `NifOsg` sets — so the quad the rasterizer would draw is scaled by the modelview along
        /// with everything else. Morrowind scales references uniformly, so one number says it.
        float scaleOf(const osg::Matrixf& place)
        {
            return osg::Vec3f(place(0, 0), place(0, 1), place(0, 2)).length();
        }
    }

    void EmitterResolver::add(const osgParticle::ParticleSystem& particles, std::span<const Shading> shading,
        const osg::Matrixf& place, ExtractionStats& stats)
    {
        // A particle's whole silhouette is its texture's alpha, so an emitter with no texture has
        // nothing to draw — not a white disc, which is what sampling nothing would give it.
        const Surface::Material* described = findDescription(shading);
        if (described == nullptr)
        {
            ++stats.mUndescribedMaterials;
            return;
        }

        const osg::Image* sprite = described->getTexture(Surface::TextureRole::Diffuse);
        if (sprite == nullptr || sprite->getFileName().empty())
            return;

        // **Registered the first time the emitter is seen, and not the first time it has a
        // particle alive.** The texture array is uploaded when the scene is built; a flame that was
        // empty at load and lights up two hundred frames later would otherwise add a texture on a
        // frame that only re-places what is already there, and index past the array it is sampling.
        //
        // Kept in a map of its own because nothing else can speak for it when the scene is swept: a
        // sprite's texture is on no material, and an emitter is not in the scene between frames.
        auto [known, arrived] = mHeld.try_emplace(&particles);
        if (arrived)
        {
            const VFS::Path::Normalized path(sprite->getFileName());
            known->second.mIndex = mScene.addTexture(path);

            // The bake is keyed on the file, so two emitters drawing with one texture share one
            // bake, and it is made when the texture is opened for upload — `SceneTextures`.
            known->second.mLighting = mScene.addBakedTexture(SpriteLightMap::keyFor(path));

            // **Held, because nothing else can name them.** An emitter is a placement and is thrown
            // away every frame, so this entry is the only lasting thing that says the sprite is in
            // use; the scene frees the slots when the sweep below lets go of them.
            mScene.holdTexture(known->second.mIndex);
            mScene.holdTexture(known->second.mLighting);
        }

        known->second.mEpoch = mEpoch;

        // **Noted now and read when the walk is over.** Whether this system has been integrated
        // this frame depends on where its `ParticleSystemUpdater` sits among its siblings — above
        // it in everything `NifOsg` builds, but that is the content's promise and not this walk's.
        // Reading after the walk has settled is what makes the question stop existing.
        mPending.push_back(Pending{
            .mParticles = &particles,
            .mPlace = place,
            .mTexture = known->second.mIndex,
            .mLighting = known->second.mLighting,

            .mLight = addsLight(shading),
            .mSprite = sprite,

        });
    }

    void EmitterResolver::flush(ExtractionStats& stats)
    {
        for (const Pending& pending : mPending)
            placeSprites(pending, stats);

        mPending.clear();
    }

    void EmitterResolver::placeSprites(const Pending& pending, ExtractionStats& stats)
    {
        const osgParticle::ParticleSystem& particles = *pending.mParticles;
        const osg::Matrixf& place = pending.mPlace;

        const float scale = scaleOf(place);

        mSpriteScratch.clear();
        const int held = particles.numParticles();
        for (int at = 0; at < held; ++at)
        {
            const osgParticle::Particle* particle = particles.getParticle(at);

            // A dead slot keeps its last position and is waiting to be born again. Drawing one is a
            // spark frozen where the previous one expired.
            if (!particle->isAlive())
                continue;

            const float radius = particle->getCurrentSize() * scale;
            if (!(radius > 0.0f))
                continue;

            // `getCurrentColor`'s alpha and `getCurrentAlpha` are two separate ramps and the
            // rasterizer multiplies them. OpenMW's `ParticleColorAffector` writes the record's
            // colour ramp into the first with its alpha forced to one and the alpha into the
            // second, so in this content the product is the second — and multiplying both is what
            // keeps that a fact about the data rather than an assumption in the reader.
            const osg::Vec4f colour = particle->getCurrentColor();
            const float alpha = colour.a() * particle->getCurrentAlpha();
            if (!(alpha > 0.0f))
                continue;

            // **Both ends through the same matrix, and the difference taken here.** `place` is where
            // the emitter stands *now*; a system carried by an actor moved between the two frames and
            // this does not know by how much, so what comes out is the particle's own travel and not
            // its travel plus its emitter's. For rain, snow and ash — which are placed in the world
            // and not on anybody — the two are the same thing, and those are the populations that
            // cross a frame fast enough for the difference to be the picture.
            const osg::Vec3f stood = particle->getPosition() * place;
            const osg::Vec3f came = particle->getPreviousPosition() * place;

            mSpriteScratch.push_back(Sprite{
                .mPosition = stood,
                .mRadius = radius,
                .mColour = osg::Vec3f(colour.r(), colour.g(), colour.b()),
                .mAlpha = alpha,
                .mMoved = stood - came,
            });
        }

        if (mSpriteScratch.empty())
            return;

        countFormat(*pending.mSprite, stats);

        // **Which way the quad faces, and `osgParticle` offers two answers.** A `BILLBOARD` system's
        // axes are the screen's and are recomputed into view space every frame, which is a disc
        // facing the eye and needs nothing carried across. A `FIXED` one's are used exactly as they
        // were authored, so its quad hangs in the world at an orientation of its own — and that is
        // the mode Morrowind's rain is built on: an X axis squashed to a tenth against a Y pointing
        // straight down is a falling streak rather than a round drop.
        //
        // Rotated into the world by the placement and not normalised, because their length is the
        // shape rather than a direction.
        osg::Vec3f across;
        osg::Vec3f upward;
        if (particles.getParticleAlignment() == osgParticle::ParticleSystem::FIXED)
        {
            across = osg::Matrixf::transform3x3(particles.getAlignVectorX(), place);
            upward = osg::Matrixf::transform3x3(particles.getAlignVectorY(), place);
        }

        mScene.addEmitter(mSpriteScratch, pending.mTexture, pending.mLight, across, upward, pending.mLighting);

        ++stats.mEmitters;
        stats.mSprites += static_cast<std::uint32_t>(mSpriteScratch.size());
    }

    void EmitterResolver::retire()
    {
        // The sprite's own references go back with the emitter that took them, which is what makes
        // an emitter leaving enough to free its textures — a frame where no mesh and no material
        // died is exactly the frame the mirror's sweep returns from without looking.
        std::erase_if(mHeld, [this](const auto& entry) {
            if (entry.second.mEpoch == mEpoch)
                return false;

            mScene.dropTexture(entry.second.mIndex);
            mScene.dropTexture(entry.second.mLighting);

            return true;
        });
    }
}
