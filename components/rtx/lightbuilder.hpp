#pragma once

#include <optional>

#include <osg/Vec3f>

#include <components/sceneutil/lightcontroller.hpp>

namespace osg
{
    class Group;
}

namespace SceneUtil
{
    class LightSource;
    struct LightCommon;
}

namespace Rtx
{
    /// One point light, placed in the world. Everything here is derived rather than read: a `LIGH`
    /// record carries a colour and a radius and no intensity at all, so there is no authored value
    /// to be faithful to. Beside `makeLight` and not in `scenedesc.hpp`, because it needs nothing
    /// else of the scene.
    struct Light
    {
        osg::Vec3f mPosition;

        /// Radiant intensity, linear, with the colour folded in. Scaled by the square of the
        /// recorded radius, which is what makes a lantern and a candle differ by their size.
        osg::Vec3f mIntensity;

        /// How far the light reaches, beyond which it contributes exactly nothing. Not the recorded
        /// radius: Morrowind's run 64 to 256 units in an interior, because a fixed falloff curve
        /// lit the lamp's own post and an ambient term filled the room. Here the lamps have to be
        /// what lights the place, so the reach is stretched while the brightness is not.
        float mReach = 0.0f;

        /// How big the glowing part is, in world units — the flame rather than the room it lights.
        /// `makeLight` derives it; zero, which a light built by hand carries, is a point. A shadow
        /// ray opens to it, so a lamp casts a penumbra as wide as it is, and it is what stops the
        /// falloff running away at the lamp itself.
        float mSourceRadius = 0.0f;

        /// How far short of the centre that ray stops. A lamp sits inside its own fitting — a
        /// lantern's frame, a sconce's bracket — so a ray that runs all the way to the light ends
        /// among that fitting and comes back fully shadowed.
        float mClearance = 0.0f;
    };

    /// Whether a `LIGH` reference standing in a cell casts at all — the game's rule, stated once.
    /// `MWClass::Light::insertObjectRendering` builds no light source for a record flagged off by
    /// default, and every other record burns where it stands, a torch on a table included:
    /// *carryable* says what an inventory may do with it and nothing about the cell. A
    /// `SceneUtil::LightCommon` because it is what the engine reduces both a `LIGH` and an ESM4
    /// `LIGH` to.
    bool castsWherePlaced(const SceneUtil::LightCommon& record);

    /// Hangs a record's light under `where`, exactly as the game hangs one on a reference. False
    /// where the record casts nothing, so a caller can drop what it built to hold one.
    ///
    /// The one place a `LIGH` becomes a light in a graph this renderer walks, for both routes to a
    /// lamp: the cell the eye stands in, and the reach around it that `DistantLights` reads out of
    /// the content files. `SceneUtil::addLight` and not `createLightSource`, so the `AttachLight`
    /// node a model may carry is honoured; a caller with no model hands over an empty group.
    ///
    /// @param exterior decides the attenuation. The reach around a cell is outdoors by definition.
    bool standLight(osg::Group& where, const SceneUtil::LightCommon& record, bool exterior);

    /// The light a `LIGH` reference casts, or nothing where it casts none — `castsWherePlaced`, and
    /// a negative light, which *subtracts* illumination in a framebuffer and is meaningless to a
    /// ray traced to an emitter.
    std::optional<Light> makeLight(const SceneUtil::LightCommon& record, const osg::Vec3f& position);

    /// The same light, from a colour and a radius rather than from a record — one conversion for
    /// the two routes, so `DistantLights` reading `LIGH` records and the mirror reading
    /// `SceneUtil::LightSource` nodes cannot disagree about how bright a candle is.
    ///
    /// @param colour linear. Null where a channel of it is negative.
    /// @param radius the recorded one. Null where it is not a size a light can have.
    std::optional<Light> makeLight(const osg::Vec3f& colour, float radius, const osg::Vec3f& position);

    /// What a light in the game's scene graph radiates this frame, in the renderer's units.
    ///
    /// The diffuse and the ambient summed, because the content uses both:
    /// `Animation::setLightEffect` gives a glow light a zero diffuse and a bright ambient, and
    /// `ActorAnimation::addHiddenItemLight` adds a white ambient on top of a carried lamp's colour.
    /// Decoded, because `SceneUtil::colourFromRGB` divides a record's bytes by 255 and stops. The
    /// recorded colours and this frame's scalars, rather than the colours the frame was written
    /// with: a flicker applied to display-encoded numbers arrives here raised to 2.4, which turns
    /// an even three tenths into eight up and five down.
    osg::Vec3f lightColour(const SceneUtil::LightSource& source, double simulationTime);

    /// How much of what a light radiates is arriving at `simulationTime` seconds, as a multiplier
    /// on its recorded colour.
    ///
    /// A `LIGH` record says *that* a light flickers or pulses and never says how, so every number
    /// behind this is chosen in the implementation. This renderer's own animation and not
    /// `SceneUtil::LightController`'s, which keeps a random walk's state and only advances where an
    /// update traversal runs it. Lands in `1 +- depth` and averages exactly one over time. A
    /// function of the clock and of `id` and of nothing else, so it is the same at a given instant
    /// however it is reached — at any frame rate, from any renderer, however many times one frame
    /// asks.
    float lightBrightness(SceneUtil::LightController::LightType type, int id, double simulationTime);
}
