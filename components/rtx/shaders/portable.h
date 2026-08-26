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

// How a shared header spells a function the shaders read and the host does not.
//
// **Metal needs `inline` and GLSL has no such keyword.** Two Metal translation units including one
// header would otherwise define the same function twice and fail to link; GLSL compiles a single
// translation unit and has nothing to say about it. The host never sees these at all — it has
// OpenSceneGraph's own vector maths and no use for a shading language's.
#ifdef __METAL_VERSION__
#define RTX_SHADER inline
#else
#define RTX_SHADER
#endif

#endif
