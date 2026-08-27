// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_STARFIELD_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_STARFIELD_GLSL

// The star field, and nothing else of the sky.
//
// **Its own file because two passes draw it.** The trace draws the rest of the sky, and this is
// drawn again by the display pass at the resolution the frame is shown at — because a point source
// is what an upscaler removes, and the only place a point survives is the resolution it is drawn
// for. So it takes the field it is asked about rather than reading the frame's, and it is the one
// piece of the sky that is not `sky.glsl`'s.

#include "scene.h"
#include "visibility.h"

#include "texturearray.glsl"

/// What the star field sends back along a ray.
///
/// **The mapping is the engine's own mesh's, measured at load rather than chosen.** The night mesh
/// lays its sheet over a dome at a fixed amount of texture per radian of sky — the same amount
/// across and up, which is what keeps a star round — so this is that unwrap without the mesh:
/// `mTile` of sky to one tile, in azimuth and in the angle down from the zenith alike. Morrowind's
/// comes to a texel a tenth of a degree wide, which is a star; the same sheet spread once over the
/// hemisphere is a third of a degree, which is a blob.
///
/// **And the fade at the horizon is the mesh's too**, not a number picked to look right: the engine
/// draws a vertex of that dome only where its authored colour is white, and the bottom ring alone is
/// not — so the field goes out between the horizon and the ring above it.
///
/// What this leaves out is the other six meshes in that file: three nebulae and the warrior, mage
/// and thief constellations, each over its own band of sky.
///
/// @param stars the field to draw, rather than the frame's — which is what lets the display pass
///        draw one it was handed on a grid the frame's camera does not describe.
/// @param blur how far this ray's cone has spread from its axis, in radians, which is what decides
///        how wide a star's edge is drawn.
vec3 starField(StarField stars, vec3 direction, float blur)
{
    if (!(stars.mFade > 0.0) || stars.mTexture == NO_TEXTURE || direction.z <= 0.0)
        return vec3(0.0);

    const float elevation = asin(clamp(direction.z, -1.0, 1.0));
    const float reaches
        = stars.mHorizon > 0.0 ? clamp(elevation / stars.mHorizon, 0.0, 1.0) : 1.0;
    if (reaches <= 0.0)
        return vec3(0.0);

    // The roll is a turn of the sphere, which in this unwrap is a shift along `u` and nothing else.
    const float azimuth = atan(direction.y, direction.x) - stars.mTurn;
    const vec2 uv = vec2(azimuth, 0.25 * TAU - elevation) / stars.mTile;

    // **A star's edge belongs to the pixel and not to the texel.** The sheet is magnified — a texel
    // is a tenth of a degree against a pixel's twentieth at the resolution this renders at — so a
    // bilinear tent spans two texels, which is nearly four pixels, and a star arrives as a soft blob
    // whose brightest pixel carries about half of it. The engine samples the same sheet the same way
    // and gets away with it because it clips: `paintAtmosphereNight` puts a white texel on the frame
    // buffer at one, so every pixel of that blob saturates and reads as a hard dot. Nothing here
    // clips. So the crossfade from one texel to the next is steepened until it happens over a pixel
    // instead of over a texel — the hardware still filters, and what changes is where inside the quad
    // it is asked. Measured at 1920 by 1080, that is 245 pixels over half brightness against 140.
    //
    // **And level zero however far the sheet is minified.** A star field is a sparse set of points,
    // and a mip level of one is those points averaged with the dark around them — one level divides
    // a star by four and takes the field apart. What a pixel wider than a texel should read is the
    // field's own mean, which `NightSky::mGlow` already states and a gather already takes; asking for
    // the level the pixel wanted instead took the pixels over a quarter from 1020 to 108.
    // Per axis rather than from the width alone, so a sheet that is not square is steepened by what
    // its own texels are worth in each direction. Floored because a parallel projection has no cone:
    // `mSpreadAngle` is nought there, and what it wants is the nearest texel, which is what a
    // steepening this hard gives.
    const vec2 size = vec2(textureSize(textures[nonuniformEXT(stars.mTexture)], 0));
    const vec2 across = max(2.0 * blur * size / stars.mTile, vec2(1.0e-6));

    const vec2 at = uv * size;
    const vec2 middle = floor(at - 0.5) + 0.5;
    const vec2 within = clamp((at - middle - 0.5) * max(1.0 / across, vec2(1.0)) + 0.5, 0.0, 1.0);

    const vec4 sheet = textureLod(textures[nonuniformEXT(stars.mTexture)], (middle + within) / size, 0.0);

    // **Premultiplied by its own alpha, which is how the engine lays this sheet on.**
    // `paintAtmosphereNight` hands the texture's alpha to a `(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)` blend,
    // and the sheet is 99% transparent — so reading the colour and dropping the alpha draws the
    // black between the stars as though it were sky.
    return (stars.mFade * reaches * STAR_RADIANCE * sheet.a) * sheet.rgb;
}


#endif
