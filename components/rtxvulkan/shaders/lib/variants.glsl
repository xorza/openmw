#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_VARIANTS_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_VARIANTS_GLSL

#include "visibility.h"

// What kind of frame this is, told to the compiler rather than to the branch predictor.
//
// **The trace is occupancy-bound, so a path nothing takes still costs the pixels that take
// another.** One kernel serving an interior with no sun, no moons and no sea spends the registers
// the moons need on every pixel of that room, and taking the moons out alone is worth a real share
// of the trace in a room no moon ray is ever traced in.
//
// **What the tuples are worth, measured** (`bench --views=all --variants=false` against the
// default, five legs each way interleaved on a hot card, 2026-09-20, the trace zone's median):
// three per cent in a daytime exterior — `one-cell-walk` 1.58 against 1.63 ms, `seyda-neen-shore`
// 2.58 against 2.66 — where the full tuple carries the moons' code the day never runs; nothing
// measurable in a room, where the legs spread by a quarter either way and the guild's medians
// came out 3.74 against 3.66. What they cost is fourteen launches more for the driver to compile
// on a cold start: eight seconds of creation and thirteen of its second compile, against 2.3 s
// and 1.1 s with the full tuple alone, so a prime (`Rtx::CodeSettle`) that settles at twenty
// seconds settles at six. Kept for the exteriors' three per cent; the switch is what measures it
// again.
//
// **Each of these stands in front of the runtime test it replaces and never in place of it.** True
// leaves the shader exactly as it was. False is set only where the test behind it already answers
// no, so what the compiler removes is dead code rather than an answer — which is what makes a
// specialized frame the same picture, byte for byte, as the one kernel drew.
//
// `Rtx::VisibilityVariant` is the other half. It reads each of these off the frame's own constants,
// and `VisibilityPass` keeps one pipeline per tuple.

/// Whether the trace counts the primary rays that hit something.
///
/// **A harness facility, so the game's module does not carry the atomic at all.** `shot` prints the
/// count, `bench` reports it and a test asserts on it, and nothing in the game ever reads it — so an
/// unconditional `atomicAdd` was a debug write compiled into the shipping kernel. Specialized rather
/// than branched on a uniform because the branch is what has to go, not just the write: with this
/// false the constant folds away and the buffer is never touched.
///
/// **Counted as misses, in the miss shader, and turned into hits on the host.** Every lane adding
/// to one word serialises at that word: a million hits took four tenths of a millisecond of the
/// trace in a room where every ray hits, which the shading of a street hid and a room's did not — a
/// harness figure that read the room's frame nine percent slow. The sky's shader runs exactly once
/// for every primary ray that ends in nothing, so the misses are the same count from the other side,
/// and a room adds nought. A ballot would add one word a subgroup instead, and a subgroup operation
/// in the launch loses the device on this driver — `visibility.rgen` holds hit objects.
layout(constant_id = 0) const bool COUNT_HITS = false;

/// Whether the sun is over the horizon: the constant half of `sunUp`, which says the rest.
layout(constant_id = 1) const bool HAS_SUN = true;

/// Whether either moon is drawn or lights anything. Both a disc with an alpha and a light with an
/// irradiance, because the sky draws one where the surfaces are lit by neither.
layout(constant_id = 2) const bool HAS_MOONS = true;

/// Whether this frame holds any water: a surface the eye can meet, or a level the eye can stand
/// under. False takes the waves, the caustics and the whole underwater column out of a room.
layout(constant_id = 3) const bool HAS_SEA = true;

/// Whether the launch sorts its threads by what they hit before the hit's shader runs —
/// `reorderThreadEXT` between the trace and the execute — and by what: `REORDER_NONE`,
/// `REORDER_SHADER` on the shader the hit names alone, `REORDER_TEXTURE` with the low bits of the
/// hit material's diffuse texture slot as the hint, for the data the shader is about to read. A
/// run's decision and not a frame's, so a constant: `RenderProfile::mReorder`, and the bench is
/// what sets it.
layout(constant_id = 4) const uint REORDER = REORDER_NONE;

#endif
