#pragma once

#include <cstdint>
#include <optional>

#include <osg/Vec3f>

#include <components/sceneutil/lightcontroller.hpp>

namespace SceneUtil
{
    class LightSource;
    struct LightCommon;
}

namespace Rtx
{
    /// One light, placed in the world: a lamp, or the fill a Light spell casts. Everything here is
    /// derived: a `LIGH` record carries a colour and a radius and no intensity at all.
    struct Light
    {
        osg::Vec3f mPosition;

        /// Radiant intensity, linear, with the colour folded in, scaled by the square of the
        /// recorded radius: what makes a lantern and a candle differ by their size.
        osg::Vec3f mIntensity;

        /// How far the light reaches, beyond which it contributes exactly nothing. Stretched from
        /// the recorded radius, because Morrowind's ran 64 to 256 units with an ambient filling the
        /// room, and here the lamps have to be what lights the place.
        float mReach = 0.0f;

        /// How big the glowing part is, in world units: the flame, which a shadow ray opens to for
        /// a penumbra as wide as it is, and what stops the falloff running away at the lamp. Zero,
        /// which a light built by hand carries, is a point.
        float mSourceRadius = 0.0f;

        /// How far short of the centre a shadow ray stops, because a lamp sits inside its own
        /// fitting and a ray that runs all the way ends among it.
        float mClearance = 0.0f;

        /// One where this light is a fill and nought where it is a lamp. A fill is a lamp whose
        /// flame is a ball `mSourceRadius` wide that a body can stand inside, lit from every side
        /// there; it is what the game means by a light whose whole output is in its ambient, and
        /// `makeFill` says which light that is. A word and not a bool, because the record is
        /// hashed whole and a bool leaves three bytes nothing wrote.
        std::uint32_t mFill = 0;
    };

    /// Whether a `LIGH` reference standing in a cell casts at all, as the game rules it: off by
    /// default casts nothing, and every other record burns where it stands, carryable or not.
    bool castsWherePlaced(const SceneUtil::LightCommon& record);

    /// The animation a record asks for, read off its four flags in the order
    /// `SceneUtil::createLightSource` reads them, so the last flag set wins there and here.
    SceneUtil::LightController::LightType animationOf(const SceneUtil::LightCommon& record);

    /// The light a `LIGH` reference casts at `simulationTime`, or nothing where it casts none:
    /// `castsWherePlaced`, and a negative light, which is meaningless to a ray traced to an emitter.
    ///
    /// **The light the walk builds from the `SceneUtil::LightSource` the game hangs on the same
    /// record, down to the last bit**, which is what lets the cell ring stand a lamp the game has
    /// not loaded and the walk take it over when it does: the radius at least sixteen, as
    /// `createLightSource` sets it; the colour decoded; the record's animation, phased by `id`;
    /// and no ambient, because `SceneUtil::addLight` hands its source none. `id` is the reference
    /// number's low word for a light no node carries, where the graph's is the node's own — so a
    /// lamp's phase changes once, on the frame its cell loads and the graph's node takes over.
    std::optional<Light> makeLight(
        const SceneUtil::LightCommon& record, const osg::Vec3f& position, double simulationTime, int id);

    /// The same light from a colour and a radius: one conversion, so a record read off the content
    /// files and a `SceneUtil::LightSource` read off the graph cannot disagree about how bright a
    /// candle is. Nothing where a channel of `colour` is negative or `radius` is no size a light
    /// can have.
    std::optional<Light> makeLight(const osg::Vec3f& colour, float radius, const osg::Vec3f& position);

    /// Whether a light in the game's scene graph is a fill: it radiates in its ambient and in
    /// nothing else. The game builds exactly one such light, the Light spell's glow
    /// (`MWRender::Animation::setLightEffect`, a diffuse of nought and an ambient of 1.5), and the
    /// only other ambient it writes rides beside a diffuse, on a lamp carried in a pack.
    bool isFill(const SceneUtil::LightSource& source);

    /// The fill an ambient-only source casts: the rasterizer's ambient term is an even brightening
    /// of everything within the radius, with no direction and no shadow, and this is that term in
    /// a ray tracer that keeps its shadows. A lamp whose flame is a ball a body and a half tall,
    /// stood on the ground at `position`, so the actor who casts the spell stands inside the
    /// light: lit from every side, because `weighLamps` blends the cosine to the centre out by how
    /// deep inside the ball a point stands, and shadowed by nothing, because the shadow ray stops
    /// at the ball. Everything outside the ball is lit by a source a body wide and shadowed as
    /// softly as that, and the reach is longer than a lamp's, because the spell's whole purpose
    /// is the pool of light around its bearer. The intensity is a lamp's of `radius`, so far off
    /// a fill is that lamp. Nothing where `colour` has a negative channel or `radius` is no size.
    std::optional<Light> makeFill(const osg::Vec3f& colour, float radius, const osg::Vec3f& position);

    /// What a light in the game's scene graph radiates this frame, in the renderer's units: the
    /// diffuse and the ambient summed, because the content uses both, and decoded, from the
    /// recorded colours and this frame's scalars rather than the colours the frame was written
    /// with, because a flicker applied to display-encoded numbers arrives raised to 2.4.
    osg::Vec3f lightColour(const SceneUtil::LightSource& source, double simulationTime);

    /// How much of what a light radiates is arriving at `simulationTime`, as a multiplier on its
    /// recorded colour. This renderer's own animation and not `SceneUtil::LightController`'s, which
    /// keeps a random walk's state: a function of the clock and of `id` and of nothing else, in
    /// `1 +- depth`, averaging one over time, the same at a given instant however many times a
    /// frame asks.
    float lightBrightness(SceneUtil::LightController::LightType type, int id, double simulationTime);
}
