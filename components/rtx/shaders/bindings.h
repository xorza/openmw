// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTX_SHADERS_BINDINGS_H
#define OPENMW_COMPONENTS_RTX_SHADERS_BINDINGS_H

#include "portable.h"

// Where set 0's inputs are bound, for the shader that declares them and the pass that writes them.
//
// **Two lists of the same numbers, kept in step by hand, is how a shader comes to read a table
// nobody wrote.** The pass builds its layout and its writes by index and the shader declares each
// binding by number; nothing but a count checked at the end connected the two, so a binding added
// in one place and forgotten in the other was a descriptor left unwritten and a dispatch reading
// whatever the slot held.
//
// Set 0 alone. The other three are a bindless texture array, the channels the trace writes and the
// air in front of the camera, and each of those is one owner's to number.
//
// **Here rather than beside the shader that declares them**, because this is where a header both
// languages read has to sit: `portable.h` is next to it, and the shader compiler is given this
// directory and no other.

#ifdef RTX_HOST

#include <cstdint>

namespace Rtx::Shaders
{
    using uint = std::uint32_t;

#endif

    /// The top-level structure every ray is traced against.
    RTX_CONST uint BIND_SCENE = 0;

    /// The atomic a specialized trace counts its hits into.
    RTX_CONST uint BIND_HITS = 1;

    /// The scene's own tables, in the order `SceneBuffers` hands them over.
    RTX_CONST uint BIND_NORMALS = 2;
    RTX_CONST uint BIND_TEXCOORDS = 3;
    RTX_CONST uint BIND_INDICES = 4;
    RTX_CONST uint BIND_MESHES = 5;
    RTX_CONST uint BIND_INSTANCES = 6;
    RTX_CONST uint BIND_MATERIALS = 7;
    RTX_CONST uint BIND_LAYERS = 8;
    RTX_CONST uint BIND_MASKS = 9;
    RTX_CONST uint BIND_LIGHTS = 10;
    RTX_CONST uint BIND_LIGHT_OFFSETS = 11;
    RTX_CONST uint BIND_LIGHT_INDICES = 12;

    /// What a pixel draws its samples from, and what a texture's own shading was measured as.
    RTX_CONST uint BIND_BLUE_NOISE = 13;
    RTX_CONST uint BIND_SHADING = 14;

    /// Where the lamps are binned, and the sprites the primary ray composites.
    RTX_CONST uint BIND_LIGHT_GRID = 15;
    RTX_CONST uint BIND_SPRITES = 16;
    RTX_CONST uint BIND_EMITTERS = 17;
    RTX_CONST uint BIND_SPRITE_TILE_OFFSETS = 18;
    RTX_CONST uint BIND_SPRITE_TILE_INDICES = 19;

    /// The one uniform: everything the frame itself says.
    RTX_CONST uint BIND_FRAME = 20;

    /// The sea's cascades and the fog's field, which are sampled rather than read.
    RTX_CONST uint BIND_WAVE_SURFACE = 21;
    RTX_CONST uint BIND_WAVE_CURVATURE = 22;
    RTX_CONST uint BIND_FOG_FIELD = 23;

    /// How many the set declares, which is the last of them and one more.
    RTX_CONST uint BIND_COUNT = 24;

#ifdef RTX_HOST
}
#endif

#endif
