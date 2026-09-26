#include "emitterresolver.hpp"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include <osg/Vec3f>
#include <osg/Vec4f>
#include <osgParticle/Particle>
#include <osgParticle/ParticleSystem>

#include <components/vfs/pathutil.hpp>

#include "colour.hpp"
#include "extractionstats.hpp"
#include "finite.hpp"
#include "lightbuilder.hpp"
#include "meantexels.hpp"
#include "refusals.hpp"
#include "result.hpp"
#include "scenedesc.hpp"
#include "shading.hpp"
#include "sprite.hpp"
#include "spritelight.hpp"
#include "surface.hpp"

namespace Rtx
{
    namespace
    {
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

        /// The image `shading` draws a system's sprites with, described into `described`, or why
        /// it names none this can draw: a system the game draws and this renderer leaves out.
        Result<const osg::Image*, std::string_view> readSprite(
            std::span<const Shading> shading, SurfaceDescription& described)
        {
            if (!describeSurface(shading, described))
                return Err{ "nothing describes its surface" };

            const osg::Image* sprite = described.getTextureUse(SurfaceMap::Diffuse).get();
            if (sprite == nullptr)
                return Err{ "its surface names no image to draw with" };

            if (sprite->getFileName().empty())
                return Err{ "its image was never a file" };

            return sprite;
        }
    }

    void EmitterResolver::describeSprite(
        const osgParticle::ParticleSystem& particles, HeldSprite& held, const std::span<const Shading> shading)
    {
        // One question, because a particle's whole silhouette is its texture's alpha and an emitter
        // this cannot name a sprite for draws nothing.
        SurfaceDescription described;
        const TextureUse& use = described.getTextureUse(SurfaceMap::Diffuse);
        const Result<const osg::Image*, std::string_view> read = readSprite(shading, described);
        if (!read.isOk())
            mScene.refusals().refuse(Refused::Emitter, particles.getName(), read.error());
        const osg::Image* sprite = read.isOk() ? read.value() : nullptr;

        held.mBlend = described.mBlend;
        held.mVertexColour = described.mVertexColour;
        held.mDiffuseColour = decodeColour(described.mDiffuseColour);
        held.mOpacity = described.mOpacity;
        if (sprite == held.mSprite)
            return;

        // The image changed under a controller, which no shipped system does but a chain that
        // animates may: the slots for the old one go back and the new one's are taken. Unnamed
        // in between, or `retire` would give a slot back twice for a system that lost its sprite.
        releaseSprite(held);
        held.mIndex = sNoIndex;
        held.mLighting = sNoIndex;
        held.mSprite = sprite;
        held.mMean = nullptr;
        if (sprite == nullptr)
            return;

        // Held, because nothing else can name them. An emitter is a placement and is thrown
        // away every frame, so this entry is the only lasting thing that says the sprite is in
        // use; the scene frees the slots when the sweep lets go of them.
        const VFS::Path::Normalized path(sprite->getFileName());
        held.mIndex = mScene.textures().take(path, *sprite, use.mWrap);
        if (held.mIndex == sNoIndex)
            mScene.refusals().refuse(
                Refused::Emitter, particles.getName(), "the texture array has no room for its image");

        // The bake is keyed on the file, so two emitters drawing with one texture share one
        // bake, and it is made when the texture is opened for upload — `SceneTextures`.
        held.mLighting = mScene.textures().addBaked(SpriteLightMap::keyFor(path));
        mScene.textures().hold(held.mLighting);
    }

    void EmitterResolver::releaseSprite(const HeldSprite& held)
    {
        mScene.textures().drop(held.mIndex);
        mScene.textures().drop(held.mLighting);
    }

    void EmitterResolver::add(const osgParticle::ParticleSystem& particles, std::span<const Shading> shading,
        const osg::Matrixf& place, const std::optional<std::size_t> glow)
    {
        // Registered the first time the emitter is seen and not the first time it has a particle
        // alive, or a flame that lights up two hundred frames later would add a texture on a frame
        // that only re-places. In a map of its own, because a sprite's texture is on no material.
        const auto [known, arrived] = mHeld.reach(&particles);
        HeldSprite& held = known->second;

        // Read where the entry arrives, and again where a link of the chain animates; every other
        // frame the reading is the one held. `Shading::mAnimatedThrough` is that scan resolved as
        // the chain is built, so no reader walks the links for it.
        if (arrived || animatedThrough(shading))
            describeSprite(particles, held, shading);

        // No image, or an image the texture table had no room for: a slot the shader reads the
        // sprite out of is what an emitter is drawn with, and it has none. `describeSprite` said
        // which.
        if (held.mSprite == nullptr || held.mIndex == sNoIndex)
            return;

        // Noted now and read when the walk is over. Whether this system has been integrated
        // this frame depends on where its `ParticleSystemUpdater` sits among its siblings — above
        // it in everything `NifOsg` builds, but that is the content's promise and not this walk's.
        // Reading after the walk has settled is what makes the question stop existing.
        mPending.push_back(Pending{
            .mParticles = &particles,
            .mPlace = place,
            .mHeld = &held,
            .mFalls = mPass.mFalls,
            .mGlow = glow,
        });
    }

    void EmitterResolver::flush(const std::span<Glow> glows)
    {
        for (const Pending& pending : mPending)
            placeSprites(pending, glows);

        mPending.clear();
    }

    void EmitterResolver::placeSprites(const Pending& pending, const std::span<Glow> glows)
    {
        ExtractionStats& stats = mPass.getStats();

        const osgParticle::ParticleSystem& particles = *pending.mParticles;
        const osg::Matrixf& place = pending.mPlace;
        HeldSprite& held = *pending.mHeld;

        const float scale = placedScale(place);

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

        // What the content's controllers simulated, so a number that is not finite is data: the
        // particle is refused and the emitter's bound measured over the rest. One that is finite
        // and draws nothing — no size, no alpha, an axis folded flat — draws nothing in the game
        // either.
        const auto readParticle
            = [&](const osgParticle::Particle& particle) -> Result<std::optional<Sprite>, std::string_view> {
            const float radius = particle.getCurrentSize() * scale;
            if (!std::isfinite(radius))
                return Err{ "a particle's size is not a finite number" };
            if (!(radius > 0.0f))
                return std::nullopt;

            // `getCurrentColor`'s alpha and `getCurrentAlpha` are two separate ramps and the
            // rasterizer multiplies them; `ParticleColorAffector` forces the first to one, and
            // multiplying both keeps that a fact about the data. Both are the vertex's, and the
            // material's mode says whether the vertex is read at all — `HeldSprite::mVertexColour`.
            // A blend that adds whole reads no alpha at all, so its sprite is all there whatever
            // its ramps say — one file in the game, and its silhouette is still its texture's.
            const bool tinted = held.mVertexColour == VertexColour::Tint;
            const osg::Vec4f vertex = particle.getCurrentColor();
            const osg::Vec3f colour = tinted ? decodeColour(vertex) : held.mDiffuseColour;
            const float opacity = tinted ? vertex.a() * particle.getCurrentAlpha() : held.mOpacity;
            const float alpha = held.mBlend == BlendKind::AddWhole ? 1.0f : opacity;
            if (!std::isfinite(alpha) || !isFinite(colour))
                return Err{ "a particle's colour is not a finite number" };
            if (!(alpha > 0.0f))
                return std::nullopt;

            const osg::Vec3f stood = particle.getPosition() * place;
            if (!isFinite(stood))
                return Err{ "a particle's place is not a finite number" };

            if (particle.getAngle() != angle)
            {
                angle = particle.getAngle();
                axis = orient(leant(authored, angle));
            }

            const float along = axis.length2();
            if (!std::isfinite(along))
                return Err{ "a particle's axis is not a finite number" };
            if (oriented && !(along > 0.0f))
                return std::nullopt;

            return Sprite{
                .mPosition = stood,
                .mRadius = radius,
                .mAxis = axis,
                .mColour = colour,
                .mAlpha = alpha,
            };
        };

        // The first reason a pass meets, reported once for the emitter: a refusal is named once
        // however many particles share it.
        std::string_view refused;

        mSpriteScratch.clear();
        const int alive = particles.numParticles();
        for (int at = 0; at < alive; ++at)
        {
            const osgParticle::Particle* particle = particles.getParticle(at);

            // A dead slot keeps its last position and is waiting to be born again. Drawing one is a
            // spark frozen where the previous one expired.
            if (!particle->isAlive())
                continue;

            const Result<std::optional<Sprite>, std::string_view> sprite = readParticle(*particle);
            if (!sprite.isOk())
            {
                if (refused.empty())
                    refused = sprite.error();
            }
            else if (sprite.value().has_value())
                mSpriteScratch.push_back(*sprite.value());
        }

        if (!refused.empty())
            mScene.refusals().refuse(Refused::Sprites, particles.getName(), refused);

        if (mSpriteScratch.empty())
            return;

        mScene.addEmitter(
            mSpriteScratch, held.mIndex, held.mBlend != BlendKind::Over, width, held.mLighting, pending.mFalls);

        ++stats.mEmitters;
        stats.mSprites += static_cast<std::uint32_t>(mSpriteScratch.size());

        // What a flame under an effect adds to the effect's lamp. The mean is read at the first
        // flame that asks and never for smoke, whose glow reads nothing of it.
        const SpriteEmitter& emitter = mScene.emitters().back();
        if (pending.mGlow.has_value() && emitter.isAdditive())
        {
            if (held.mMean == nullptr)
                held.mMean = &mMeans.of(*held.mSprite);

            glows[*pending.mGlow].addSprites(emitter, mSpriteScratch, held.mMean->mColour);
        }
    }

    void EmitterResolver::retire()
    {
        // After the flush, because a pending emitter points into the map this erases from.
        assert(mPending.empty() && "a sweep with emitters noted and not yet placed");

        // The sprite's own references go back with the emitter that took them, which is what makes
        // an emitter leaving enough to free its textures — a frame where no mesh and no material
        // died is exactly the frame the mirror's sweep returns from without looking.
        mHeld.retire([this](const HeldSprite& held) { releaseSprite(held); });
    }
}
