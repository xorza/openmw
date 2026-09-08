#pragma once

#include <optional>

#include <osg/Vec3f>

#include <components/sceneutil/lightcontroller.hpp>

#include "light.hpp"

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
    /// Whether a `LIGH` reference standing in a cell casts at all.
    ///
    /// **The game's rule, and the one place the renderer states it.**
    /// `MWClass::Light::insertObjectRendering` builds no light source for a record flagged **off by
    /// default** — an unlit brazier is a mesh and nothing else — and every other record burns where
    /// it stands, a torch on a table included: *carryable* says what an inventory may do with it and
    /// nothing about the cell it lies in. Every route to a light reads this, so a graph built here
    /// and a record read here cannot come to place different lamps.
    ///
    /// **A description and not a record**, because `SceneUtil::LightCommon` is what the engine
    /// reduces both a `LIGH` and an ESM4 `LIGH` to, and it is what `Terrain::ObjectStorage::getLight`
    /// hands over — so the reach around a cell and the cell itself ask the one question.
    bool castsWherePlaced(const SceneUtil::LightCommon& record);

    /// Hangs a record's light under `where`, exactly as the game hangs one on a reference. False
    /// where the record casts nothing, so a caller can drop what it built to hold one.
    ///
    /// **The one place a `LIGH` becomes a light in a graph this renderer walks.** There are two
    /// routes to a lamp — the cell the eye stands in, whose references the game itself would place,
    /// and the reach around it, whose lamps `DistantLights` reads out of the content files because
    /// `Terrain::pagedType` stands no model for them. Everything they can share is here: whether the
    /// record burns, which mask the node is marked with, and `SceneUtil::addLight` doing the rest.
    /// What is left to a caller is where the light hangs, which is the only thing the two differ
    /// about.
    ///
    /// **`SceneUtil::addLight` and not `createLightSource`**, so the `AttachLight` node a model may
    /// carry is honoured — a lantern's flame sits at the wick. A caller with no model hands over a
    /// group holding none, and the light lands on the group itself.
    ///
    /// @param exterior decides the attenuation. The reach around a cell is outdoors by definition.
    bool standLight(osg::Group& where, const SceneUtil::LightCommon& record, bool exterior);

    /// The light a `LIGH` reference casts, or nothing where it casts none.
    ///
    /// A record that does not cast where it stands — `castsWherePlaced` — places no light, and a
    /// **negative** one is refused further down. It *subtracts* illumination — a trick
    /// available to a renderer accumulating into a framebuffer and meaningless to anything that
    /// traces a ray to an emitter — and this describes it as the negative colour the game's scene
    /// graph builds, so the overload below refuses both paths with one test.
    std::optional<Light> makeLight(const SceneUtil::LightCommon& record, const osg::Vec3f& position);

    /// The same light, from a colour and a radius rather than from a record.
    ///
    /// **One conversion and two routes to it**, which is the point: `DistantLights` reads a cell's
    /// `LIGH` records for the reach the paging leaves dark, and the mirror reads the
    /// `SceneUtil::LightSource` nodes the graph already holds. The two must not come to disagree
    /// about how bright a candle is.
    ///
    /// @param colour linear. `lightColour` and `decodeColour` are the two ways of getting one there.
    ///        Null where a channel of it is negative, which is what a light that subtracts looks
    ///        like by either route.
    /// @param radius the recorded one. Null where it is not a size a light can have.
    std::optional<Light> makeLight(const osg::Vec3f& colour, float radius, const osg::Vec3f& position);

    /// What a light in the game's scene graph radiates this frame, in the renderer's units.
    ///
    /// **Both terms, because the content uses both.** A fixed-function pipeline had a diffuse and an
    /// ambient because it had two different things to do with them; a ray tracer has one, and it is
    /// their sum. Two places in the game write the ambient and mean light by it:
    /// `Animation::setLightEffect` gives a glow light a zero diffuse and a bright ambient, so every
    /// Light spell and every enchanted item radiates exactly nothing to anything that reads the
    /// diffuse alone; and `ActorAnimation::addHiddenItemLight` adds a white one on top of the
    /// record's own colour, so a lamp carried in a pack lights its bearer more, and whiter, than the
    /// same lamp on a table.
    ///
    /// **Decoded, because what the game hands over is not linear.** `SceneUtil::colourFromRGB`
    /// divides a record's bytes by 255 and stops, so a `SceneUtil::LightSource` carries the file's
    /// own numbers exactly as the record does — and this is the same decode
    /// `makeLight(const SceneUtil::LightCommon&)` makes, which is what keeps a candle in a played frame as
    /// bright as the same candle in a screenshot.
    ///
    /// **The recorded colours and this frame's scalars, rather than the colours the frame was
    /// written with.** A flicker and an actor's fade are changes in what the light *radiates*, and
    /// the numbers the graph carries are display-encoded — so the rasterizer's own scaling of them
    /// arrives here raised to 2.4, which turns an even flicker of three tenths into a lopsided one
    /// of eight tenths up and five down. The scalars are taken apart from the colours and applied
    /// after the decode, where a half means a half.
    osg::Vec3f lightColour(const SceneUtil::LightSource& source, double simulationTime);

    /// How much of what a light radiates is arriving at `simulationTime` seconds, as a multiplier
    /// on its recorded colour.
    ///
    /// A `LIGH` record says *that* a light flickers or pulses and never says how: it carries a
    /// colour, a radius and four flags, and no amplitude, rate or phase anywhere. So every number
    /// behind this is chosen in the implementation, and each says what it was chosen from.
    ///
    /// **This renderer's own animation, and not the one the rasterizer draws.**
    /// `SceneUtil::LightController` walks a light's brightness about at fifteen steps a second and
    /// keeps the walk's state, which is a picture of a flame rather than a model of one — and it
    /// only advances where an update traversal runs it, which is not everywhere this renderer
    /// works. Lands in `1 +- depth` and averages exactly one over time, so a light that animates is
    /// as bright on average as the same light standing still.
    ///
    /// **A function of the clock and of `id`, and of nothing else.** No state a frame advances,
    /// which is what makes it the same at a given instant however it is reached: at any frame rate,
    /// from any renderer, in the harness, and however many times one frame asks. What separates two
    /// candles standing together is the light's own id.
    float lightBrightness(SceneUtil::LightController::LightType type, int id, double simulationTime);
}
