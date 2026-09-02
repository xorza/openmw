// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_TEXTUREARRAY_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_TEXTUREARRAY_GLSL

// The scene's textures, and nothing else.
//
// **Its own file because a second pass samples them.** The trace has this by way of everything else
// it is handed; the display pass draws the sky's own points at the resolution they are shown at, and
// needs the sheet and none of the rest of `bindings.glsl`. A set number is a fact shared with
// whichever passes bind it, so it is written once.

// **The extension travels with the declaration**, because what needs it is the indexing rather than
// the pass: a shader that includes this and forgets the line fails to compile, which is the failure
// worth having.
#extension GL_EXT_nonuniform_qualifier : require

/// Every texture the scene loaded, indexed by the slot a material, a layer or an emitter names.
///
/// **A slot is qualified where it indexes and never where it is passed.** Neighbouring lanes hit
/// different materials over most of a frame, so the descriptor read has to be a waterfall — and what
/// tells the driver to emit one is a `NonUniform` decoration on the access chain itself.
/// `nonuniformEXT` applied to a function *argument* decorates the argument and stops there: the
/// chain built inside the callee comes out bare, and the driver may then read one lane's descriptor
/// for the whole wave. That is a wrong texture on some lanes of some waves, which looks like nothing
/// at all until it does. Measured before this rule, 28 of the 44 chains into this array were
/// undecorated and every one of them was on the surface path; `spirv-val` passes either way and the
/// validation layers say nothing.
layout(set = 1, binding = 0) uniform sampler2D textures[];

/// What each texture already has painted into it, `SHADING_EXTENT` squared, at the slot of the
/// texture it was measured on and through the same sampler, which wraps as the texture does.
///
/// **A binding of its own and not slots between the textures**, because `coneLod` measures the
/// array it reads for the level a cone resolves, and a map interleaved with the textures is one it
/// would measure. A slot with a texture always has a map, neutral where nothing could estimate one.
/// Stored over the range `SHADING_FLOOR` to `SHADING_CEILING`, which `paintedLight` decodes.
layout(set = 1, binding = 1) uniform sampler2D shadingMaps[];

#endif
