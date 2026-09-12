#include "emitterresolver.hpp"

#include <cstdint>

#include <osgParticle/Particle>
#include <osgParticle/ParticleSystem>

#include <components/surface/material.hpp>
#include <components/vfs/pathutil.hpp>

#include "colour.hpp"
#include "extractionstats.hpp"
#include "scenedesc.hpp"
#include "shading.hpp"
#include "spritelight.hpp"

namespace Rtx
{
    namespace
    {
        /// The uniform scale a placement carries, as the length of its first basis row: a sprite's
        /// size is in the particle system's own coordinates, and Morrowind scales references
        /// uniformly.
        float scaleOf(const osg::Matrixf& place)
        {
            return osg::Vec3f(place(0, 0), place(0, 1), place(0, 2)).length();
        }

        /// `axis` turned by the rotation one particle carries, composed the way `osgParticle`
        /// composes it — `Matrix::makeRotate(angle.x, X, angle.y, Y, angle.z, Z)` — so a raindrop
        /// leans into the wind exactly as the rasterizer leans it.
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

        // One question and one count, because a particle's whole silhouette is its texture's alpha
        // and an emitter this cannot name a sprite for draws nothing.
        Surface::Material described;
        const osg::Image* sprite
            = describeSurface(shading, described) ? described.getTexture(Surface::TextureRole::Diffuse) : nullptr;

        if (sprite == nullptr || sprite->getFileName().empty())
        {
            ++stats.mSpritelessEmitters;
            return;
        }

        // Registered the first time the emitter is seen and not the first time it has a particle
        // alive, or a flame that lights up two hundred frames later would add a texture on a frame
        // that only re-places. In a map of its own, because a sprite's texture is on no material.
        const auto [known, arrived] = mHeld.reach(&particles);
        if (arrived)
        {
            const VFS::Path::Normalized path(sprite->getFileName());
            known->second.mIndex = mScene.textures().add(path);

            // The bake is keyed on the file, so two emitters drawing with one texture share one
            // bake, and it is made when the texture is opened for upload — `SceneTextures`.
            known->second.mLighting = mScene.textures().addBaked(SpriteLightMap::keyFor(path));

            // Held, because nothing else can name them. An emitter is a placement and is thrown
            // away every frame, so this entry is the only lasting thing that says the sprite is in
            // use; the scene frees the slots when the sweep below lets go of them.
            mScene.textures().hold(known->second.mIndex);
            mScene.textures().hold(known->second.mLighting);
        }

        // Noted now and read when the walk is over. Whether this system has been integrated
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

        // Which way the quad faces: a `BILLBOARD` system's is a disc facing the eye, and a `FIXED`
        // one's hangs in the world as authored, which is how Morrowind's rain is a falling streak.
        // Both axes or neither, because one alone describes no plane.
        const bool oriented = particles.getParticleAlignment() == osgParticle::ParticleSystem::FIXED
            && particles.getAlignVectorX().length2() > 0.0f && particles.getAlignVectorY().length2() > 0.0f;

        // How wide the streak is against its own length, which is all the across axis says here: the
        // march swings the width about the axis to meet the ray rather than committing the quad to
        // the plane the content picked. Neither the particle's rotation nor the placement can change
        // that length, so it is the emitter's and is taken once.
        const float width = oriented ? particles.getAlignVectorX().length() : 0.0f;
        const osg::Vec3f authored = oriented ? particles.getAlignVectorY() : osg::Vec3f();

        // Turned by the placement and not scaled by it, because a sprite's radius already
        // carries the scale: the quad reaches `mAxis * mRadius`, so scaling both would square it.
        // A placement that collapses to nothing gives a zero axis, and its sprites have no radius
        // to draw with either.
        const float inverseScale = scale > 0.0f ? 1.0f / scale : 0.0f;
        const auto orient
            = [&](const osg::Vec3f& axis) { return osg::Matrixf::transform3x3(axis, place) * inverseScale; };

        // The angle one run of particles shares, and the axis it gave. A shooter fires every
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
            // rasterizer multiplies them; `ParticleColorAffector` forces the first to one, and
            // multiplying both keeps that a fact about the data.
            const osg::Vec4f colour = particle->getCurrentColor();
            const float alpha = colour.a() * particle->getCurrentAlpha();
            if (!(alpha > 0.0f))
                continue;

            // Both ends through the same matrix, so what comes out is the particle's own travel and
            // not its emitter's too. For rain, snow and ash the two are the same thing, and those
            // are the populations that cross a frame fast enough for the difference to be the
            // picture.
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
                .mColour = decodeColour(colour),
                .mAlpha = alpha,
                .mMoved = stood - came,
            });
        }

        if (mSpriteScratch.empty())
            return;

        stats.mFormats.count(*pending.mSprite);

        mScene.addEmitter(mSpriteScratch, pending.mTexture, pending.mLight, width, pending.mLighting);

        ++stats.mEmitters;
        stats.mSprites += static_cast<std::uint32_t>(mSpriteScratch.size());
    }

    void EmitterResolver::retire()
    {
        // The sprite's own references go back with the emitter that took them, which is what makes
        // an emitter leaving enough to free its textures — a frame where no mesh and no material
        // died is exactly the frame the mirror's sweep returns from without looking.
        mHeld.retire([this](const HeldSprite& held) {
            mScene.textures().drop(held.mIndex);
            mScene.textures().drop(held.mLighting);
        });
    }
}
