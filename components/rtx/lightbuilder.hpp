#pragma once

#include <optional>
#include <span>

#include <osg/BoundingBox>
#include <osg/BoundingSphere>
#include <osg/Matrixf>
#include <osg/Vec3f>

#include <components/sceneutil/lightcontroller.hpp>

#include "light.hpp"
#include "sprite.hpp"

namespace SceneUtil
{
    class LightSource;
    struct LightCommon;
}

namespace Rtx
{
    struct Material;

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

    /// What a magic effect's glowing sheets and flames add up to: the one lamp they make together.
    /// Opened where the walk enters an effect, handed every additive sheet the walk meets under it
    /// and every additive emitter read once the walk is over, and closed into a `Light` after both.
    ///
    /// **One lamp for the effect and not one for each sheet**, because a burst is twenty sheets
    /// stood at one place, and twenty lamps of a fireball's reach are twenty times the entries
    /// in the light grid, which coarsens every cell in the room for the length of the burst.
    struct Glow
    {
        /// What the sheets radiate, summed, per unit of area — `addSheet` says the arithmetic.
        osg::Vec3f mRadiance;

        /// The ball every sheet stands in, in the world: the shell `makeLight` has them radiate
        /// from, and the sheets' alone, so a spray of sparks round a burst widens the lamp and not
        /// the sheets' worth. Invalid until a sheet is added.
        osg::BoundingSpheref mSheets;

        /// What the flames radiate times the discs they radiate from, summed over every sprite and
        /// divided by pi — `addSprites` says the arithmetic.
        osg::Vec3f mDiscs;

        /// The ball every sheet and every sprite stands in: the lamp's source. Invalid until
        /// something is added.
        osg::BoundingSpheref mBall;

        /// Whether the game hung a light of its own on the effect — a bolt's `LightSource` — which
        /// is then the effect's light, and this glow lights nothing: a lamp made of the picture
        /// beside the lamp the game meant would count the bolt twice.
        bool mLit = false;

        /// Adds one sheet of the effect: a mesh of `box` wearing `worn`, stood by `place` and shown
        /// at `fade`. Nothing where `worn` does not add.
        ///
        /// **What a sheet radiates is what `additiveAlong` adds for it**, per unit of area on
        /// average: its map's mean texel under the material's tint and opacity, times the glow the
        /// material states at `EMISSIVE_INTENSITY` — the white ambient the game gives an effect is
        /// in that glow already, `MaterialResolver::describe` says where. The light the sheet
        /// reflects is not in it, because that light is some lamp's and a lamp of it would count
        /// that lamp twice. The material's own colours and never the vertex's: no effect in the
        /// game tints its sheets by vertex, and a sheet that did would light as its material says.
        ///
        /// **The ball is the mesh's own box, half its widest side about its centre, scaled and
        /// stood by the placement** — the box the table kept as the vertices arrived,
        /// `MeshRange::mBounds`, so no sheet is measured here and no triangle walked. Half the
        /// widest side and not the box's diagonal, which is a sphere's radius by root three; and
        /// the box in the mesh's own frame and never its corners carried into the world, because a
        /// burst's sheets are billboards the walk turns to the eye, and a world box of a turning
        /// square grows and shrinks by root two as the camera moves — a lamp that moves every
        /// frame, which the light grid rebuilds for. The size the spell states is the size the game
        /// drew the burst at: `CastSpell::explodeSpell` stands the area effect at twice its area,
        /// and the ball is what that came to.
        void addSheet(const Material& worn, const osg::BoundingBoxf& box, const osg::Matrixf& place, float fade);

        /// Adds one emitter of the effect: `sprites`, which are `emitter`'s, drawn with a texture
        /// whose mean texel is `mean`. Nothing where the emitter does not add.
        ///
        /// **What a sprite radiates is what `spritesAlong` adds for it**: its texel under the
        /// particle's colour and alpha, at `FLAME_INTENSITY`, with the texture's mean texel —
        /// already weighted by its own alpha, `MeanTexel::mColour` — standing for the texel. A
        /// billboard shows the whole of its disc to every direction, and flames that add hide
        /// nothing of one another, so a cloud of them is a source of intensity `pi * r^2 * L`
        /// summed over its discs, in every direction. Exact for what the march adds sprite by
        /// sprite; what it is over by is the saturation the march applies to a stack, `1 - prod(1 -
        /// g)`, which a sum does not have.
        ///
        /// **The ball is the emitter's own**, `SpriteEmitter::mCentre` and `mReach`, which the
        /// scene measured over these same sprites.
        void addSprites(const SpriteEmitter& emitter, std::span<const Sprite> sprites, const osg::Vec3f& mean);

        /// The lamp this is, or nothing where nothing glowed or the game lit the effect itself: a
        /// fill whose ball is the effect's own, so a burst of fifty feet lights whatever stands
        /// inside it from every side and shadows it with nothing, and everything outside it by a
        /// source fifty feet wide.
        ///
        /// **The sheets' intensity is a closed shell's of their ball's radius glowing at their
        /// radiance, `2 * pi * L * R^2`**: each face of a shell leaves `pi * L` per unit of area, a
        /// Lambertian exitance, both faces of `4 * pi * R^2` are crossed by every ray that reaches
        /// it, and a point of intensity `I` sheds `4 * pi * I`. Exact for the fire burst, which is
        /// a sphere, and twice the truth for a lone flat sheet, whose two faces are half a shell's
        /// area. The flames' is `pi` times what `addSprites` summed. The intensity is derived and
        /// not read off a record, so what a burst is worth against the lamps of the room is set by
        /// eye — `sGlowGain` and `sGlowReachScale` say how, and a burst is the one lamp in the game
        /// whose whole purpose is the room around it.
        std::optional<Light> makeLight() const;
    };

}
