// `#pragma once` everywhere else in this tree, and an include guard here: `glslc` warns
// "'#pragma once' : not implemented" and carries on, so a header included twice by one
// shader would redefine everything in it.
#ifndef OPENMW_COMPONENTS_RTX_SHADERS_SPRITESHADE_H
#define OPENMW_COMPONENTS_RTX_SHADERS_SPRITESHADE_H

#include "hosttypes.h"
#include "portable.h"

// How many layers of its own emitter stand between each sprite and a light, counted each frame.
//
// **A column of smoke has a sunlit side and a shaded side, and nothing per sprite can give it
// one.** The bake shadows a puff by its own texture and the wrap gives it a side, but the eye
// composites twenty of them over a pixel and their coverage-weighted mean washes what each did into
// one flat colour. What a column needs is shadow *between* sprites: what the ones nearer the sun
// leave of it for the ones behind. That is a question about the emitter's sprites and the light
// alone, so it is answered once per frame and the trace reads one number per sprite per light.
//
// **Counted in layers and not in transmittance, because this does not hold the texture.** A sprite
// that stands in the light's path to another counts for its own fade — one layer for a whole puff,
// a fraction for a wisp — and the trace thins the light by what one layer of that texture hides on
// average, which is the texture's coarsest level. `(1 - mean) ^ layers` is exact where every layer
// is whole, and for a faded one it is the approximation the bake already takes.
//
// **A grid across the light and not every pair**, so that a storm's thousands cost per sprite what
// a candle's dozen do. Sprites are walked from the light outward; each reads what has been laid
// down at its own point and then lays its own disc down on top. The grid is the emitter's reach,
// square, and a disc lands on it as an antialiased footprint: whole inside, a one-cell ramp at the
// rim — which keeps the disc's area to first order — and its area as a point where it is too small
// to reach a cell's centre.
//
// Only what covers and faces the eye is shaded. A flame emits and shadows nothing of its own kind,
// and a rain streak is a thin thing seen by what passes through it.
//
// **On the device and not the host, and Vivec is why.** The same arithmetic in a loop on the
// processor is a fifth of a crowded view's CPU frame — the one place in the corpus where the
// processor, not the card, decides how long a frame takes. There is no host copy kept beside this
// as a reference: two implementations of one computation are two things to keep in step, and
// what a layer count is belongs in one place.
//
// **One workgroup per emitter per light, and one lane per cell of the grid.** A sprite reads what
// the sprites nearer the light have laid down, then every lane adds its own cell's coverage of that
// sprite's disc — so the sequence over sprites is serial, which is what the answer depends on, and
// the parallelism is across the grid, which is where it changes nothing. `spriteshade.comp` holds
// the grid's own size and everything else only it reads.
//
// **The depth order is sorted into a buffer and not into shared memory**, which is what lets a run
// be any length. Shared memory caps it, a cap needs something to shade what is past the cap, and
// that something is the second implementation this has none of.

#ifdef RTX_HOST
namespace Rtx::Shaders
{
#endif

    /// How many lights each emitter is shaded against, which is what the dispatch and the order
    /// buffer are both scaled by.
    ///
    /// **The one number both sides need.** Everything else the shading is made of — the grid, the
    /// widest sprite it allows, which of the two lights a workgroup has — is the shader's alone and
    /// is spelled there.
    const uint SPRITE_SHADE_LIGHTS = 2u;

    /// What the dispatch is handed.
    struct SpriteShadeConstants
    {
        uint64 mSprites;
        uint64 mEmitters;

        /// Scratch: one key per sprite per light, which is where each run is sorted.
        ///
        /// **A buffer and not shared memory**, so that a run of any length is shaded by the one
        /// implementation. Light `l`'s key for sprite `s` sits at `l * mCount + s`, so the two
        /// workgroups an emitter gets never write the same word. Nothing reads it after the
        /// dispatch, and nothing carries it between frames.
        uint64 mOrder;

        /// Unit, toward the sun. The sky is straight up and is not carried.
        vec3 mToSun;

        /// How many emitters there are. The dispatch is this times `SPRITE_SHADE_LIGHTS`.
        uint mEmitterCount;

        /// How many sprites there are, which is the stride between the two lights in `mOrder`.
        uint mCount;
    };

#ifdef RTX_HOST

    static_assert(sizeof(SpriteShadeConstants) == 48, "SpriteShadeConstants must be scalar-packed on every side");
}

#endif

#endif
