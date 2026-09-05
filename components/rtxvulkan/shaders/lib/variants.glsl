// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_VARIANTS_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_VARIANTS_GLSL

// What kind of frame this is, told to the compiler rather than to the branch predictor.
//
// **The trace is occupancy-bound, so a path nothing takes still costs the pixels that take
// another.** One kernel served an interior with no sun, no moons and no sea: the registers the
// moons need are registers every pixel of that room did without, and taking the moons out alone was
// measured at half a millisecond in a room no moon ray is ever traced in.
//
// **Each of these stands in front of the runtime test it replaces and never in place of it.** True
// leaves the shader exactly as it was. False is set only where the test behind it already answers
// no, so what the compiler removes is dead code rather than an answer — which is what makes a
// specialized frame the same picture, byte for byte, as the one kernel drew.
//
// `Rtx::VisibilityVariant` is the other half. It reads each of these off the frame's own constants,
// and `VisibilityPass` keeps one pipeline per tuple.

#include "visibility.h"

/// Whether the trace counts the primary rays that hit something.
///
/// **A harness facility, so the game's module does not carry the atomic at all.** `shot` prints the
/// count, `bench` reports it and a test asserts on it, and nothing in the game ever reads it — so an
/// unconditional `atomicAdd` was a debug write compiled into the shipping kernel. Specialized rather
/// than branched on a uniform because the branch is what has to go, not just the write: with this
/// false the constant folds away and the buffer is never touched.
layout(constant_id = 0) const bool COUNT_HITS = false;

/// Whether the sun is over the horizon. `mSunIrradiance` is nought exactly where it is not, and
/// fades to that across dusk rather than stepping, so an interior and a night are the same answer.
layout(constant_id = 1) const bool HAS_SUN = true;

/// Whether either moon is drawn or lights anything. Both a disc with an alpha and a light with an
/// irradiance, because the sky draws one where the surfaces are lit by neither.
layout(constant_id = 2) const bool HAS_MOONS = true;

/// Whether this frame holds any water: a surface the eye can meet, or a level the eye can stand
/// under. False takes the waves, the caustics and the whole underwater column out of a room.
layout(constant_id = 3) const bool HAS_SEA = true;

/// How the trace sorts its threads between the traversal and the shader that resolves what they
/// found.
///
/// **Not one of the four above, because it is not a fact about the frame.** The tuple is what a
/// dusk or a doorway moves; this is fixed for the life of the pass, the way `COUNT_HITS` is — the
/// harness names it on the command line, so each form of the reorder is a build of one pipeline
/// rather than a pipeline of its own. The `REORDER_*` values in `visibility.h` are what this takes,
/// and `Rtx::Reorder` is the host's side of them.
layout(constant_id = 4) const uint REORDER = REORDER_OFF;

/// Whether the trace counts the see-through surfaces each primary ray crosses.
///
/// **A whole traversal, and off wherever nobody asked.** `COUNT_HITS` costs an atomic on the pixels
/// that hit something; this costs a second walk of the structure on every pixel, so it cannot ride
/// with it — a benchmark under it would be measuring the census rather than the frame. `shot
/// --crossings` is the one thing that turns it on.
layout(constant_id = 5) const bool COUNT_CROSSINGS = false;

#endif
