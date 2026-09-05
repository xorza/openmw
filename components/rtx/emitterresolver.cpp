#include "emitterresolver.hpp"

#include <cstdint>

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

        /// `axis` turned by the rotation one particle carries.
        ///
        /// **The rasterizer's own matrix, composed the way it composes it.** `osgParticle` turns
        /// both of a quad's axes by `Matrix::makeRotate(angle.x, X, angle.y, Y, angle.z, Z)` before
        /// it draws them, and `Weather::RainShooter` is what leans a raindrop into the wind that
        /// way. Built from the three angles here rather than derived from the wind, so the ray
        /// tracer cannot lean a drop differently from the renderer beside it.
        osg::Vec3f leant(const osg::Vec3f& axis, const osg::Vec3f& angle)
        {
            if (angle == osg::Vec3f())
                return axis;

            osg::Matrixf turn;
            turn.makeRotate(angle.x(), osg::Vec3f(1.0f, 0.0f, 0.0f), angle.y(), osg::Vec3f(0.0f, 1.0f, 0.0f), angle.z(),
                osg::Vec3f(0.0f, 0.0f, 1.0f));

            return axis * turn;
        }
    }

    void EmitterResolver::add(
        const osgParticle::ParticleSystem& particles, std::span<const Shading> shading, const osg::Matrixf& place)
    {
        ExtractionStats& stats = mPass.getStats();

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

        known->second.mEpoch = mPass.mEpoch;

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

    void EmitterResolver::flush()
    {
        for (const Pending& pending : mPending)
            placeSprites(pending);

        mPending.clear();
    }

    void EmitterResolver::placeSprites(const Pending& pending)
    {
        ExtractionStats& stats = mPass.getStats();

        const osgParticle::ParticleSystem& particles = *pending.mParticles;
        const osg::Matrixf& place = pending.mPlace;

        const float scale = scaleOf(place);

        // **Which way the quad faces, and `osgParticle` offers two answers.** A `BILLBOARD` system's
        // axes are the screen's and are recomputed into view space every frame, which is a disc
        // facing the eye and needs nothing carried across. A `FIXED` one's are used exactly as they
        // were authored, so its quad hangs in the world at an orientation of its own — and that is
        // the mode Morrowind's rain is built on: an X axis squashed to a tenth against a Y pointing
        // straight down is a falling streak rather than a round drop.
        //
        // **Both axes or neither**, because one of them alone describes no plane.
        const bool oriented = particles.getParticleAlignment() == osgParticle::ParticleSystem::FIXED
            && particles.getAlignVectorX().length2() > 0.0f && particles.getAlignVectorY().length2() > 0.0f;

        // How wide the streak is against its own length, which is all the across axis says here: the
        // march swings the width about the axis to meet the ray rather than committing the quad to
        // the plane the content picked. Neither the particle's rotation nor the placement can change
        // that length, so it is the emitter's and is taken once.
        const float width = oriented ? particles.getAlignVectorX().length() : 0.0f;
        const osg::Vec3f authored = oriented ? particles.getAlignVectorY() : osg::Vec3f();

        // **Turned by the placement and not scaled by it**, because a sprite's radius already
        // carries the scale: the quad reaches `mAxis * mRadius`, so scaling both would square it.
        // A placement that collapses to nothing gives a zero axis, and its sprites have no radius
        // to draw with either.
        const float inverseScale = scale > 0.0f ? 1.0f / scale : 0.0f;
        const auto orient
            = [&](const osg::Vec3f& axis) { return osg::Matrixf::transform3x3(axis, place) * inverseScale; };

        // **The angle one run of particles shares, and the axis it gave.** A shooter fires every
        // particle it makes with the same angle, so a frame of rain is one or two runs — and this is
        // what keeps it from building a rotation matrix per drop.
        osg::Vec3f angle;
        osg::Vec3f axis = orient(authored);

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

            if (particle->getAngle() != angle)
            {
                angle = particle->getAngle();
                axis = orient(leant(authored, angle));
            }

            mSpriteScratch.push_back(Sprite{
                .mPosition = stood,
                .mRadius = radius,
                .mAxis = axis,
                .mColour = osg::Vec3f(colour.r(), colour.g(), colour.b()),
                .mAlpha = alpha,
                .mMoved = stood - came,
            });
        }

        if (mSpriteScratch.empty())
            return;

        countFormat(*pending.mSprite, stats);

        mScene.addEmitter(mSpriteScratch, pending.mTexture, pending.mLight, width, pending.mLighting);

        ++stats.mEmitters;
        stats.mSprites += static_cast<std::uint32_t>(mSpriteScratch.size());
    }

    void EmitterResolver::retire()
    {
        // The sprite's own references go back with the emitter that took them, which is what makes
        // an emitter leaving enough to free its textures — a frame where no mesh and no material
        // died is exactly the frame the mirror's sweep returns from without looking.
        std::erase_if(mHeld, [this](const auto& entry) {
            if (entry.second.mEpoch == mPass.mEpoch)
                return false;

            mScene.dropTexture(entry.second.mIndex);
            mScene.dropTexture(entry.second.mLighting);

            return true;
        });
    }
}
