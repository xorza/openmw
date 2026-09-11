#pragma once

#include <cstdint>

#include <osg/Vec3f>
#include <osg/Vec4f>
#include <osg/Vec4ub>

#include <components/surface/colour.hpp>

namespace Rtx
{
    /// A colour as the content files store one, decoded.
    ///
    /// Morrowind's colours are display-encoded, and the light transport downstream is linear. The
    /// two differ most in the middle, so mid grey is where a renderer that skips this is most
    /// obviously wrong and where a test pins it.
    ///
    /// **The one crossing, and every colour entering this renderer takes it.** The game states a
    /// colour in four components — a `Vec4f`, four bytes, or a packed word — and the trace holds
    /// three in light, so the narrowing and the decode are one step. Writing that narrowing by hand
    /// is the mistake this exists to stop, and it was made twice: a particle's ramp reached the
    /// sprite table in the space the file wrote it, and a material's tint and glow reached the
    /// shader the same way. The second of those narrowed nothing at all — it copied a `Vec4f` to a
    /// `Vec4f` — which is why `Surface::Colour` gives the description a type a renderer cannot
    /// copy out of.
    osg::Vec3f decodeColour(std::uint32_t packed);

    /// The same decode, for a colour something else has already unpacked to `[0, 1]`.
    ///
    /// **What the game hands over is display-encoded too.** OpenMW's own renderer works in that
    /// space from end to end and never converts, so every colour read off a light, a fog, the sky
    /// or a model's vertex is the file's own number divided by 255 — and a ray tracer that took it
    /// as linear would be as wrong there as it would be reading the record itself. The alpha is
    /// dropped: nothing downstream has a use for it.
    osg::Vec3f decodeColour(const osg::Vec4f& encoded);

    /// The same again, for the four bytes `NifOsg` and `Terrain` write a vertex colour as.
    osg::Vec3f decodeColour(const osg::Vec4ub& encoded);

    /// And for what a content file said a surface is, which carries its space in its type.
    osg::Vec3f decodeColour(const Surface::Colour& encoded);
}
