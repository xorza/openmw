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

        /// Half the sprite's width in world units, which is what `osgParticle` means by a size.
        float mRadius = 0.0f;

        /// The streak's own axis in the world, per unit of `mRadius`, or zero for a sprite that
        /// faces the eye. Per particle, because `Weather::RainShooter` leans each drop into the wind
        /// it was fired under. Not normalised, because its length is the shape.
        osg::Vec3f mAxis;

        /// Linear, and already carrying wherever the particle's own colour ramp has reached.
        osg::Vec3f mColour{ 1.0f, 1.0f, 1.0f };

        /// What the particle's own fade left of it, multiplied into the texture's alpha at the hit.
        float mAlpha = 1.0f;
    };

    /// One particle system: what its sprites are drawn with, and a sphere that holds all of them,
    /// which is the whole spatial structure because one rejection throws a small emitter away for
    /// almost every pixel.
    struct SpriteEmitter
    {
        osg::Vec3f mCentre;

        /// Far enough from `mCentre` to contain every sprite in the range, rim included.
        float mReach = 0.0f;

        /// Where they sit in `SceneDesc::sprites`, laid end to end as the emitter placed them.
        Run mSprites;

        /// The sprite texture, or `sNoIndex` where the emitter had none, which draws nothing.
        Index mTexture = sNoIndex;

        /// What that texture's alpha leaves of the light crossing a sprite — a `SpriteLightMap` —
        /// or `sNoIndex` for one lit as a flat card.
        Index mLighting = sNoIndex;

        /// `SRC_ALPHA, ONE`: a flame, which adds light and hides nothing behind it. The rest blend
        /// over, which is smoke and needs its colour ramp to fade it.
        bool mAdditive = false;

        /// Whether its sprites fall from the sky — the rain box, a driven storm — and so are kept
        /// out from under a roof. `SceneExtractor::extractFalling` is the walk that says so.
        bool mFalls = false;

        /// How wide this emitter's quads are against their own axis, per unit of `Sprite::mRadius`,
        /// or nought for sprites that face the eye: Morrowind's rain is an X axis squashed to a tenth
        /// against a Y axis pointing straight down. The length and not the direction, because the
        /// march swings the width about the sprite's own axis to meet the ray.
        float mWidth = 0.0f;
    };
}
