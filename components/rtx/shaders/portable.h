// `#pragma once` everywhere else in this tree, and an include guard here: `glslc` warns
// "'#pragma once' : not implemented" and carries on, so a header included twice by one
// shader would redefine everything in it.
#ifndef OPENMW_COMPONENTS_RTX_SHADERS_PORTABLE_H
#define OPENMW_COMPONENTS_RTX_SHADERS_PORTABLE_H

// How the three languages that read the shared structures differ, in one place.
//
// GLSL needs none of it: `vec3` is a builtin and a program-scope `const` is already where it wants
// to be. C++ says its half inside each header's own namespace, because putting `vec3` and `uint` at
// global scope would be a poor trade for a shared header. Metal's half is here.

// **Metal is C++ too**, so `__cplusplus` is defined inside a Metal shader and cannot be what tells
// the host apart from one. Everything that is the host's alone — the standard library, OpenSceneGraph,
// the namespace — hangs off this instead.
#if defined(__cplusplus) && !defined(__METAL_VERSION__)
#define RTX_HOST 1
#endif

#ifdef __METAL_VERSION__

// **Metal gives `float3` sixteen bytes.** Every other side packs it to twelve, so a structure shared
// with them names the packed spelling or stops being the same bytes. The packed types promote to
// `float3` for arithmetic, so only the fields have to say it.
using vec2 = packed_float2;
using vec3 = packed_float3;
using vec4 = packed_float4;
using uvec3 = packed_uint3;

// Metal requires a program-scope variable to name its address space, where GLSL and C++ have none.
#define RTX_CONST constant

#else
#define RTX_CONST const
#endif

// How a shared header spells a function every side of it defines for itself.
//
// **Metal and C++ need `inline` and GLSL has no such keyword.** Two translation units including one
// header would otherwise define the same function twice and fail to link; GLSL compiles a single
// translation unit and has nothing to say about it.
//
// **What the host takes is the scalar arithmetic and not the shading maths.** A shading language's
// vectors are OpenSceneGraph's on this side and it has no use for a second set — but a curve a
// shader is fitted to is a curve a test has to be able to call, and a fit nobody can check is a
// magic number.
#if defined(__METAL_VERSION__) || defined(RTX_HOST)
#define RTX_SHADER inline
#else
#define RTX_SHADER
#endif

// A value whose rounding is load-bearing: GLSL's `precise` forbids fusing its multiplies and
// adds, so every compile of the shader agrees on it. `rayAt` says what that is for. Nothing on the
// host or in Metal, where the arithmetic is written as it is read.
#if defined(__METAL_VERSION__) || defined(RTX_HOST)
#define RTX_PRECISE
#else
#define RTX_PRECISE precise
#endif

#endif
