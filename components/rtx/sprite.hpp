#pragma once

#include <osg/Vec3f>

#include "index.hpp"
#include "run.hpp"

namespace Rtx
{
    /// One live particle, drawn as a disc facing the eye.
    ///
    /// **A particle system carries no triangles at all** — the sprites are the whole of the drawing —
    /// so nothing here reaches an acceleration structure. The layer is marched against the primary
    /// ray and composited instead, which is also what lets it blend in depth order without the
    /// candidate loop an alpha-blended hit would cost traversal.
    struct Sprite
    {
        osg::Vec3f mPosition;

        /// Half the sprite's width in world units, which is what `osgParticle` means by a size: its
        /// quad runs from `-size` to `+size` about the particle and its bounds are expanded by it.
        float mRadius = 0.0f;

        /// The streak's own axis in the world, per unit of `mRadius` — or **zero for a sprite that
        /// faces the eye**, which is nearly every one. `SpriteEmitter::mWidth` is the other half of
        /// the shape and is the emitter's, because a rotation cannot change it.
        ///
        /// **Per particle, because the rotation is.** `osgParticle` turns both of a quad's axes by
        /// the angle the particle carries before it draws them, and `Weather::RainShooter` is what
        /// leans a raindrop into the wind with it — so two drops fired under different winds hang at
        /// different angles in one frame, and an axis held once for the emitter drew the whole storm
        /// falling straight down.
        ///
        /// **Not normalised**, because its length is the shape: rain's is a whole radius against a
        /// width of a tenth, which is what makes a drop a streak.
        osg::Vec3f mAxis;

        /// Linear, and already carrying wherever the particle's own colour ramp has reached.
        osg::Vec3f mColour{ 1.0f, 1.0f, 1.0f };

        /// What the particle's own fade left of it, multiplied into the texture's alpha at the hit.
        float mAlpha = 1.0f;

        /// Where the particle stood on the previous frame, less where it stands now — see
        /// `Shaders::GpuSprite::mMoved` for why it is the difference that is carried.
        ///
        /// **The particle's own answer.** `osgParticle` keeps a previous position per particle for
        /// its own line rendering, so nothing here has to track a particle across frames or care
        /// that births and deaths reshuffle the array.
        osg::Vec3f mMoved;
    };

    /// One particle system: what its sprites are drawn with, and a sphere that holds all of them.
    ///
    /// **The sphere is the whole spatial structure and it is enough.** A light is asked for by a
    /// shading *point*, which the uniform grid answers in a lookup; an emitter is asked for by a
    /// whole *ray*, which would have to walk that grid cell by cell. There are tens of emitters in a
    /// cell against hundreds of lamps and each is small, so one rejection throws an emitter away for
    /// almost every pixel of the frame.
    struct SpriteEmitter
    {
        osg::Vec3f mCentre;

        /// Far enough from `mCentre` to contain every sprite in the range, rim included.
        float mReach = 0.0f;

        /// Where they sit in `SceneTables::mSprites`, laid end to end as the emitter placed them.
        Run mSprites;

        /// The sprite texture, or `sNoIndex` where the emitter had none — which draws nothing, since
        /// a particle's whole silhouette is in that texture's alpha.
        Index mTexture = sNoIndex;

        /// What that texture's alpha leaves of the light crossing a sprite — a `SpriteLightMap` —
        /// or `sNoIndex` for one lit as a flat card.
        Index mLighting = sNoIndex;

        /// `SRC_ALPHA, ONE`: a flame, which adds light and hides nothing behind it. The rest blend
        /// over, which is smoke and needs its colour ramp to fade it.
        bool mAdditive = false;

        /// How wide this emitter's quads are against their own axis, per unit of `Sprite::mRadius`
        /// — or **nought for sprites that face the eye**, which is nearly every emitter in the game.
        ///
        /// `osgParticle` draws a particle as `position ± axisX * size ± axisY * size` and offers two
        /// ways of choosing those axes. A `BILLBOARD` system's are the screen's, transformed into
        /// view space every frame — that is a disc facing the eye and needs nothing carried here. A
        /// `FIXED` one's are used as they were authored, so the quad hangs in the world at an
        /// orientation of its own, and Morrowind's rain is the reason the mode exists: an X axis
        /// squashed to a tenth against a Y axis pointing straight down is a falling streak rather
        /// than a round drop.
        ///
        /// **The length of that X axis and not its direction**, because the march swings the width
        /// about the sprite's own axis to meet the ray rather than committing it to the plane the
        /// content picked. `Sprite::mAxis` carries the rest of the shape, and carries it per
        /// particle because a particle's own rotation turns it.
        float mWidth = 0.0f;
    };

    /// Everything the renderer needs to know about a world, with no Vulkan and no scene graph in it.
    ///
    /// Lights come from ESM `Light` records rather than from the graph: `NifOsg` never reads
    /// `NiLight`, so a model carries none — a candle's mesh and the light it casts arrive by
    /// different routes and are placed by the same reference.
    ///
    /// Deliberately dumb: it appends and it dedups paths, and nothing else. Deciding that two
    /// drawables are the same mesh belongs to whoever is reading the scene graph, which knows what
    /// identity means there; this type would have to guess.
}
