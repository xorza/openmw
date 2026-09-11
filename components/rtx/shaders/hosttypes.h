// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTX_SHADERS_HOSTTYPES_H
#define OPENMW_COMPONENTS_RTX_SHADERS_HOSTTYPES_H

#include "portable.h"

// What a shared structure is spelled in, said once.
//
// **Sixteen headers said it for themselves, and each said a trimmed part of it.** A header that
// grew a `vec4` had to grow an alias and an OpenSceneGraph include beside it, and a header that
// lost its last `vec2` kept both — so the block drifted per file and a reader had to check every
// copy against every other. There is nothing in it that belongs to one structure rather than to
// all of them.
//
// **The namespace is reopened rather than nested.** Each header still writes
// `namespace Rtx::Shaders` around its own contents, which is where a reader looks for what a name
// belongs to. This only puts the aliases in that namespace first.

#ifdef RTX_HOST

#include <cstdint>

#include <osg/Vec2f>
#include <osg/Vec2ui>
#include <osg/Vec3f>
#include <osg/Vec3ui>
#include <osg/Vec4f>

namespace Rtx::Shaders
{
    using vec2 = osg::Vec2f;
    using vec3 = osg::Vec3f;
    using vec4 = osg::Vec4f;
    using uvec2 = osg::Vec2ui;
    using uvec3 = osg::Vec3ui;
    using uint = std::uint32_t;
    using uint64 = std::uint64_t;
}

#else

// **Asked for here rather than by each shader that needs it.** Five headers addressed a table by a
// 64-bit pointer and each declared the extension and defined the spelling for itself, then undefined
// it at the end — an order that worked because of which header happened to include which. Said once
// and left standing, a translation unit cannot get it wrong.
//
// **A shader that spells no 64-bit type gains nothing by it.** `composite.comp` and
// `wavecompose.comp` reach this header and neither declares `OpCapability Int64`: the extension
// permits the type and the compiler emits the capability only where one is used.
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

#define uint64 uint64_t

#endif

#endif
