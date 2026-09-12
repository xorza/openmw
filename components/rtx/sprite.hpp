#pragma once

#include <osg/Vec3f>

#include "runs.hpp"

namespace Rtx
{
    /// One live particle, drawn as a disc facing the eye. Nothing here reaches an acceleration
    /// structure: the layer is marched against the primary ray and composited, which blends in
    /// depth order without the candidate loop an alpha-blended hit would cost traversal.
    struct Sprite
    {
        osg::Vec3f mPosition;

        /// Half the sprite's width in world units, which is what `osgParticle` means by a size: its
        /// quad runs from `-size` to `+size` about the particle and its bounds are expanded by it.
        float mRadius = 0.0f;

        /// The streak's own axis in the world, per unit of `mRadius` — or zero for a sprite that
        /// faces the eye, which is nearly every one. Per particle, because `Weather::RainShooter`
        /// leans each drop into the wind it was fired under, and an axis held once for the emitter
        /// drew the whole storm falling straight down. Not normalised, because its length is the
        /// shape. `SpriteEmitter::mWidth` is the other half.
        osg::Vec3f mAxis;

        /// Linear, and already carrying wherever the particle's own colour ramp has reached.
        osg::Vec3f mColour{ 1.0f, 1.0f, 1.0f };

        /// What the particle's own fade left of it, multiplied into the texture's alpha at the hit.
        float mAlpha = 1.0f;

        /// Where the particle stood on the previous frame, less where it stands now — see
        /// `Shaders::GpuSprite::mMoved`. `osgParticle` keeps a previous position per particle for
        /// its own line rendering, so nothing here tracks a particle across frames.
        osg::Vec3f mMoved;
    };

    /// One particle system: what its sprites are drawn with, and a sphere that holds all of them.
    /// The sphere is the whole spatial structure: an emitter is asked for by a whole ray, there are
    /// tens of them in a cell and each is small, so one rejection throws it away for almost every
    /// pixel.
    struct SpriteEmitter
    {
        osg::Vec3f mCentre;

        /// Far enough from `mCentre` to contain every sprite in the range, rim included.
        float mReach = 0.0f;

        /// Where they sit in `SceneDesc::sprites`, laid end to end as the emitter placed them.
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
        /// — or nought for sprites that face the eye, which is nearly every emitter in the game. A
        /// `FIXED` system's quad hangs in the world as authored, and Morrowind's rain is an X axis
        /// squashed to a tenth against a Y axis pointing straight down. The length of that X axis
        /// and not its direction, because the march swings the width about the sprite's own axis to
        /// meet the ray.
        float mWidth = 0.0f;
    };

    /// Everything the renderer needs to know about a world, with no Vulkan and no scene graph in
    /// it. It appends and it dedups paths, and nothing else: deciding that two drawables are the
    /// same mesh belongs to whoever is reading the scene graph.
}
