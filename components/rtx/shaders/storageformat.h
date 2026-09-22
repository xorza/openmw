#ifndef OPENMW_COMPONENTS_RTX_SHADERS_STORAGEFORMAT_H
#define OPENMW_COMPONENTS_RTX_SHADERS_STORAGEFORMAT_H

#include "portable.h"

// The texel layouts a shader declares on an image it loads or stores, one name for both sides.
//
// **A resource's format is then one line.** `#define GBUFFER_ALBEDO STORAGE_RGBA16F` is both the
// qualifier the shader declares and the enumerator the host creates the image from, so the two
// cannot come apart — and they must not: a view whose format is not the declared one is undefined
// behaviour, and the layers call it a warning, which fails nothing.
//
// **What stays paired by hand is the language's fact and not a pass's.** A qualifier names one
// layout, and nothing a pass decides changes which, so each pair below is written once and never
// revisited when a channel changes width.
//
// **The host's half names no API.** The backend is where an enumerator becomes a format a device
// creates.
//
// A macro because a layout qualifier is a token GLSL reads before it parses anything, and the
// preprocessor is the one thing both languages share.

#ifdef RTX_HOST

#include <cstdint>

namespace Rtx::Shaders
{
    /// The texel layout of an image a shader loads or stores, named for its GLSL qualifier.
    enum class StorageFormat : std::uint8_t
    {
        Rgba8,
        R16,
        R16f,
        R32f,
        Rg16f,
        Rg32f,
        Rgba16f,
        Rgba32f,
    };
}

#define STORAGE_RGBA8 ::Rtx::Shaders::StorageFormat::Rgba8
#define STORAGE_R16 ::Rtx::Shaders::StorageFormat::R16
#define STORAGE_R16F ::Rtx::Shaders::StorageFormat::R16f
#define STORAGE_R32F ::Rtx::Shaders::StorageFormat::R32f
#define STORAGE_RG16F ::Rtx::Shaders::StorageFormat::Rg16f
#define STORAGE_RG32F ::Rtx::Shaders::StorageFormat::Rg32f
#define STORAGE_RGBA16F ::Rtx::Shaders::StorageFormat::Rgba16f
#define STORAGE_RGBA32F ::Rtx::Shaders::StorageFormat::Rgba32f

#else

#define STORAGE_RGBA8 rgba8
#define STORAGE_R16 r16
#define STORAGE_R16F r16f
#define STORAGE_R32F r32f
#define STORAGE_RG16F rg16f
#define STORAGE_RG32F rg32f
#define STORAGE_RGBA16F rgba16f
#define STORAGE_RGBA32F rgba32f

#endif

#endif
