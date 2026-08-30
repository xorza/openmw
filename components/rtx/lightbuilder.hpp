#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include <osg/Vec3f>
#include <osg/Vec4f>

#include <components/esm3/loadcell.hpp>
#include <components/misc/constants.hpp>
#include <components/sceneutil/lightcontroller.hpp>
#include <components/sky/timeofday.hpp>

#include "fogbuilder.hpp"
#include "scenedesc.hpp"

namespace ESM
{
    struct Light;
    struct Region;
}

namespace SceneUtil
{
    class LightSource;
}

namespace Rtx
{
    /// Whether a `LIGH` reference standing in a cell casts at all.
    ///
    /// **The game's rule, and the one place the renderer states it.**
    /// `MWClass::Light::insertObjectRendering` builds no light source for a record flagged **off by
    /// default** — an unlit brazier is a mesh and nothing else — and every other record burns where
    /// it stands, a torch on a table included: *carryable* says what an inventory may do with it and
    /// nothing about the cell it lies in. Both routes to a light read this, so a graph built here
    /// and a record read here cannot come to place different lamps.
    bool castsWherePlaced(const ESM::Light& record);

    /// The light a `LIGH` reference casts, or nothing where it casts none.
    ///
    /// A record that does not cast where it stands — `castsWherePlaced` — places no light, and a
    /// **negative** one is refused further down. It *subtracts* illumination — a trick
    /// available to a renderer accumulating into a framebuffer and meaningless to anything that
    /// traces a ray to an emitter — and this describes it as the negative colour the game's scene
    /// graph builds, so the overload below refuses both paths with one test.
    std::optional<Light> makeLight(const ESM::Light& record, const osg::Vec3f& position);

    /// The same light, from a colour and a radius rather than from a record.
    ///
    /// **One conversion and two callers**, which is the point: the harness reads a cell's `LIGH`
    /// records and the game reads the `SceneUtil::LightSource` nodes its own scene graph already
    /// holds, and the two must not come to disagree about how bright a candle is.
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
    /// `makeLight(const ESM::Light&)` makes, which is what keeps a candle in a played frame as
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

    /// What to hold a measured exposure back by, for a sky delivering this much light.
    ///
    /// One where the hour delivers a full sun's worth or more, falling from there. `Daylight`'s own
    /// field says why an hour has to be told to the exposure rather than measured out of the frame.
    float exposureBias(const osg::Vec3f& sunIrradiance, const osg::Vec3f& ambient);

    /// What a weather says about the sky at one hour, in the renderer's own units.
    ///
    /// Both renderers reach these six numbers by their own route — one reports what a live weather
    /// system settled on, the other derives them from the content files at an hour it was told — and
    /// then hand them to `makeSkylight` rather than assembling a sun themselves.
    struct SkyReading
    {
        /// Where the disc stands, unit. `Sky::sunAt`.
        osg::Vec3f mSunPosition = osg::Vec3f(0.0f, 0.0f, 1.0f);

        /// How much of the sun is over the horizon. `Sky::sunShareAt`.
        float mSunShare = 0.0f;

        /// How much of it a layer standing above the ground still has — `sunShareAloft`.
        ///
        /// **Two askers and one weather.** A cloud deck keeps the sun after the ground has lost it,
        /// and everything else it reads is the same: the same place, and the same colour, because
        /// the content's sunset is keyed on the hour. Nought where nothing stands above the ground
        /// to ask, which is a frame with no deck in it.
        float mSunShareAloft = 0.0f;

        /// The weather's `Sun_*_Color` at this hour, linear — Morrowind's own, night blue and all.
        osg::Vec3f mSunColour;

        /// The weather's `Ambient_*_Color` at this hour, linear.
        osg::Vec3f mAmbient;

        /// What the disc is painted with, linear. `Sky::sunDiscAt`.
        osg::Vec3f mDiscColour = osg::Vec3f(1.0f, 1.0f, 1.0f);

        /// The weather's `Glare_View`: how much of the sun it lets through.
        float mGlare = 1.0f;
    };

    /// The sky's light, in the two forms a tracer can use it: one that comes from somewhere, and one
    /// that does not.
    struct Skylight
    {
        Sun mSun;

        /// The same sun as a layer above the ground sees it, out of `SkyReading::mSunShareAloft`.
        Sun mSunAloft;

        /// What a path is terminated with, which is the weather's own ambient plus whatever of the
        /// sun is not over the horizon. `makeSkylight` says why.
        osg::Vec3f mAmbient;
    };

    /// The sky's light, out of what a weather says — and the one place a sun is allowed to be built.
    ///
    /// **A sun below the horizon is not a sun, and this is where that becomes impossible to say.**
    /// Morrowind never switches its sunlight off: `WeatherManager` reads a colour off the same ramp
    /// all night — `Sun_Night_Color` is `59, 97, 176` and is brighter in blue than most of the day —
    /// and turns off only the *sprite*. Its renderer could afford that, because a directional light
    /// with no visible source looks like nothing in particular in a rasterized frame. Traced, it is
    /// a sun: it casts hard shadows that swing back across the ground all night, off a disc that
    /// retraces its own arc while nothing is drawn at the end of it.
    ///
    /// So what the file calls the night's sun is put where light with no direction belongs — the
    /// ambient — and the sun keeps only what is over the horizon. **The two halves are complements**,
    /// so the total is continuous through dusk rather than stepping when the sun goes out: the share
    /// that is still up lights as a direction, and the share that is not lights with none. That is
    /// also what twilight is.
    ///
    /// What it lights with is a quarter of the irradiance over pi. A directional source delivers, averaged over
    /// every orientation a surface could take, a quarter of its irradiance — the mean of `max(0,
    /// cos)` over the sphere — and a uniform hemisphere of radiance `L` delivers `pi L` to all of
    /// them, so `E / 4pi` is the same light with the direction taken out of it. Nothing is invented
    /// and nothing is lost; a night simply stops having a sun in it.
    Skylight makeSkylight(const SkyReading& sky);

    /// How high the cloud layer stands, in world units.
    ///
    /// **The one number in the sky that is chosen rather than read.** Nothing in Morrowind states
    /// it: the cloud mesh gives its height in tiles of its own sheet and no metre anywhere. Five
    /// hundred metres is a stratocumulus base and is the reference implementation's own choice, made
    /// where it decides how large a cloud's shadow reads.
    ///
    /// It settles what `sunShareAloft` reads the sun at, how wide a tile of the sheet is across the
    /// world, and so how large a shadow the deck casts.
    ///
    /// **A world height and not a height over the eye**, which is what a shadow needs: a layer that
    /// rose with the camera would cast a shadow that moved with it. What still follows the eye is
    /// the deck's *extent*, because the fade rings are the mesh's own and are measured from there.
    inline constexpr float sCloudAltitude = 500.0f * Constants::UnitsPerMeter;

    /// A sun out of a weather's reading and however much of the disc the asker can see.
    ///
    /// **`makeSkylight` is the ground's asker and no longer the only one.** A cloud deck stands above
    /// the ground's horizon and keeps the sun after it has set down here, so it reads the same
    /// weather at its own share — `sunShareAloft`. Everything else about the sun is the same for
    /// both of them: it is in the same place, and the content's own sunset colour is keyed on the
    /// hour rather than on how much air the beam crossed.
    Sun sunAbove(const SkyReading& sky, float share);

    /// How much of the sun a layer standing over the ground still has at `hour`.
    ///
    /// **The engine's sunset is a clock and not a horizon**, which is the whole of the shape here.
    /// `Sky::sunShareAt` ramps on the hour and `Sky::sunAt` puts the disc level with the horizon at
    /// exactly `mNightStart`, so nothing anywhere takes an elevation — a layer that keeps the sun
    /// past the ground's horizon cannot be handed a lower one and is handed a different hour instead.
    ///
    /// How far the clock moves is the dip a layer that high sees, over the time the disc takes to
    /// fall it: 0.718 degrees at 5.35 game minutes on the shipped fourteen-hour day. Against the
    /// two-hour dusk that is a shift of 4.5%, and at the instant the ground's sun goes out the layer
    /// still holds 8.7% of it.
    ///
    /// **Its day is the ground's widened at both ends rather than moved**, because a layer that sees
    /// the sun lower sees it earlier in the morning as well as later in the evening.
    ///
    /// **And nothing is done to the colour**, because `Sun_Sunset_Color` is the content's own
    /// reddening and is keyed on the same hour. A layer's dusk differs from the ground's in when it
    /// ends, not in what colour it is, and `airTransmittance` over the top would be the same sunset
    /// stated twice.
    ///
    /// The layer is the cloud deck, which is the only thing this renderer puts above the ground.
    float sunShareAloft(float hour, const Sky::TimeOfDaySettings& times);

    /// What the sky delivers to a surface facing it, and how much of that it is never drawn with.
    struct SkyBudget
    {
        /// The whole of it, as a radiance: the gradient, the night's sheets and `mFill` together.
        ///
        /// **What lights anything the sky stands over**, which so far is the cloud deck. A deck
        /// hangs under this and sends a share of it back down, and the ground under the deck is lit
        /// by whatever got past.
        osg::Vec3f mMean;

        /// What the sky delivers as light over and above the colour it is drawn with.
        ///
        /// **Morrowind lights its night with an ambient and this renderer lights it with a sky, and
        /// the two are an order apart.** The engine puts `Ambient_<weather>_Night_Color` on every
        /// surface directly; the ray tracer throws bounce rays at the dome instead and reads
        /// `Sky_<weather>_Night_Color`, which is a tenth of it — so the ground came out ten times
        /// short of the night the content describes, against a sky drawn exactly as bright as ever.
        ///
        /// So the sky is held to what the weather says a night is worth. A gradient that runs
        /// linearly in `sin(elevation)` delivers what a uniform sky of `horizon / 3 + 2 * zenith / 3`
        /// would, the night's sheets add their own mean on top of that, and whatever the ambient
        /// asks for beyond the two is this. **It is light and not a colour**: nothing draws it,
        /// because Morrowind does not draw it either — its ambient is on the surfaces and never in
        /// the sky.
        ///
        /// **Every layer that lights comes out of the same figure**, which is what keeps a night
        /// from brightening each time one more of them starts lighting: the stars did not, and now
        /// they do, and the night is where it was.
        ///
        /// **Nought by day, with no hour asked.** A weather's daylight sky outruns its daylight
        /// ambient in all three channels, so the rule bites only where the content puts the light
        /// somewhere the sky cannot carry it — which is night, and the deepest part of dusk.
        osg::Vec3f mFill;
    };

    /// Reads both off one weather, so nothing can hold two ideas of what a sky is worth.
    ///
    /// @param sheets what the night sky's own layers add — `Shaders::StarField::mGlow`.
    SkyBudget skyBudget(
        const osg::Vec3f& horizon, const osg::Vec3f& zenith, const osg::Vec3f& sheets, const osg::Vec3f& ambient);

    /// The sun and the sky at one hour, as the content files describe them.
    ///
    /// Every colour here is a fallback setting the game reads for itself, and the sun's arc, its
    /// four-point ramps and its disc all come from `components/sky` — the same arithmetic the
    /// weather manager runs, so a harness frame and a game frame stand under one sky rather than
    /// under two that were written to agree.
    struct Daylight
    {
        Sun mSun;

        /// The same sun as a layer above the ground sees it — `Skylight::mSunAloft`, carried through
        /// so a caller that took its whole sky from an hour has the deck's half of it too.
        Sun mSunAloft;

        /// Sky radiance, linear, at the horizon and overhead. The horizon is the weather's fog
        /// colour, which is also the air the cloud deck hangs in and is lifted off.
        osg::Vec3f mSkyHorizon;
        osg::Vec3f mSkyZenith;

        /// What an exterior gets in place of a cell's `AMBI`, which only interiors carry — the
        /// weather's own ambient, and across dusk the sun's light with its direction taken away.
        osg::Vec3f mAmbient;

        /// How far the stars have come out: the engine's `Stars` ramp at this hour, before the
        /// weather's glare is taken off it.
        float mStarFade = 0.0f;

        /// What to hold the measured exposure back by, from this hour alone. One leaves it alone.
        ///
        /// **Night is a thing the world knows and not a thing the picture can measure.** A histogram
        /// has no absolute anchor: it normalises whatever it is shown toward the key, so a midnight
        /// and a noon come out within a few per cent of each other and the renderer has no night in
        /// it at any hour. The weather does know the hour, so it says how dark the hour is here and
        /// the exposure pass is told rather than left to guess. `settle` derives it.
        float mExposureBias = 1.0f;

        /// The weather's own air.
        ///
        /// **Its colour is `mSkyHorizon`, and the same read fills both.** Morrowind records one
        /// colour for the fog and for the sky's lower half because they are the same thing at two
        /// distances — the horizon *is* fog — so a ray that reaches nothing and a ray through a mile
        /// of air have to arrive at the same answer.
        Fog mFog;
    };

    /// A weather's index, as `MWWorld::WeatherManager` registers them and the shader's `WEATHER_*`
    /// name them, or nothing for a name that is none of the ten.
    ///
    /// **One table, two callers**, which is the point it shares with `makeLight`: the game hands the
    /// renderer a weather's script id and the harness hands it a name off a command line, and a
    /// frame taken either way has to be under the same sky.
    std::optional<std::uint32_t> weatherIndex(std::string_view weather);

    /// Refuses a weather whose keys the configuration never provided.
    ///
    /// **`Fallback::Map` answers an allowed key nobody wrote with nought**, so a weather the ini
    /// importer was never run for renders with no fog, no wind and black colours, and nothing says
    /// why — which is how two of the ten went unnoticed on a box that never ran the importer. A
    /// missing thing is a hard failure naming it: this names the weather and the first key it lacks.

    ///
    /// @param floats,strings the tables to look in — `Fallback::Map`'s own, or a test's.
    void requireWeather(std::string_view weather, const std::map<std::string, float, std::less<>>& floats,
        const std::map<std::string, std::string, std::less<>>& strings);

    /// The name that index spells, for whoever has to hand one back to `makeDaylight`. Empty for
    /// an index past the ten.
    std::string_view weatherName(std::uint32_t weather);

    /// The weather one step either side of this one, skipping any the region never gets.
    ///
    /// **A region does not see all ten.** A `REGN` record carries ten chances that add to a hundred,
    /// in the order `WEATHER_*` names them, and a zero is a weather that never happens there: the
    /// ash wastes never snow, Solstheim never has an ashstorm, and offering either is offering a sky
    /// the game would not produce.
    ///
    /// A null region — an interior, or a cell whose record names none — offers all ten, and so does
    /// a region whose chances are all zero, since the alternative is a step that goes nowhere.
    std::uint32_t nextRegionWeather(const ESM::Region* region, std::uint32_t weather, bool forward);

    /// How much of the sun that weather lets through — `Weather_<name>_Glare_View`, which dims a sun
    /// disc under an overcast and keeps the stars in behind one.
    float glareView(std::string_view weather);

    /// Where a storm drives what it carries, for an observer standing at `observer`.
    ///
    /// **Ash and blight blow off Red Mountain.** `apps/openmw/mwworld/weather.cpp:47` aims the
    /// direction from the volcano at whoever is standing in it, flattened to the ground — which is
    /// why an ashstorm comes at the player's face wherever they walk, and why this needs a position
    /// at all. Every other weather takes the wind's own bearing and does not.
    ///
    /// The game reports what its own weather system computed, since it has a player to ask about;
    /// this is the same rule for a harness that has only a camera.
    osg::Vec3f stormDirection(std::uint32_t weather, const osg::Vec3f& observer);

    /// The daylight a named weather casts at `hour`, on a twenty-four hour clock.
    ///
    /// @param weather a weather's name as the fallback settings spell it — "Clear", "Cloudy",
    ///        "Overcast" and the rest. **A name that is none of the ten throws** `std::logic_error`
    ///        out of the fallback map, which whitelists its keys one weather at a time; whoever
    ///        takes a name from outside should put it through `weatherIndex` before this.
    Daylight makeDaylight(std::string_view weather, float hour);

    /// The daylight partway between two weathers, at `blend` from the first to the second.
    ///
    /// **What `WeatherManager::calculateTransitionResult` does, and it blends the same things.** Each
    /// weather's numbers are read at the hour and then mixed — the fog's recorded *depth* among them
    /// rather than the extinction it becomes, because those are two different curves and the engine
    /// converts after blending.
    Daylight makeDaylight(std::string_view from, std::string_view to, float blend, float hour);

    /// A room's light, out of its own `AMBI` record — with `makeDaylight`, the other of the two
    /// places a `Daylight` is built, and the one the game and the harness both light a room by.
    ///
    /// **The record, and not the rasterizer's reading of it.** `RenderingManager::configureAmbient`
    /// lifts an interior's ambient to `minimum interior brightness` before its own lights see it,
    /// which balances a falloff curve this renderer does not have, and `openmw-rtxtool` has no
    /// rasterizer to read. So both hand over the four numbers the cell wrote, in the record's own
    /// type, and this is the one reading of them.
    ///
    /// **A room has a sun, and it is the game's.** `configureAmbient` lights every interior with the
    /// record's sunlight colour as a directional light from `Sky::roomSun`, at full share and never
    /// at night.
    ///
    /// The sky is the fog colour at both ends, since a room has no dome and its air stands in
    /// wherever a ray gets out. The stars are nought and the exposure bias is one, which is what the
    /// game holds a room at.
    ///
    /// @param nightEye what the Night-Eye effect adds to every channel of the ambient, in the
    ///        file's own space — which is where `RenderingManager::updateAmbient` adds it, so it is
    ///        added before the decode here as well. Nothing for the harness, which casts no spells.
    Daylight makeRoomLight(const ESM::Cell::AMBIstruct& room, const osg::Vec3f& nightEye = osg::Vec3f());

    /// What the air leaves of a body in the sky, per channel.
    ///
    /// **The one thing between an eye and a moon, and the reason a moon can rise at all here.** The
    /// engine draws no moon under `Moons_<name>_Fade_End_Angle` — thirty degrees for Secunda, forty
    /// for Masser — because a lit quad over its own fogged dome reads as a sticker. A renderer that
    /// traces the air does not need that: the air takes a low moon out on its own, and gradually.
    ///
    /// Rayleigh optical depth at the three sRGB primaries, times the air mass along the slant path.
    /// Both are published: the depth is `0.008569 λ^-4` with its usual correction, which comes to
    /// 0.068, 0.097 and 0.221 at the zenith, and the air mass is Kasten and Young's fit, which is
    /// 37.92 at the horizon against one overhead. So a moon comes up a deep red ember, is orange at
    /// five degrees and is itself by thirty.
    ///
    /// **Not the sun's, though the same air is over it.** `Sun_Disc_Sunset_Color` already reddens
    /// that disc and `sunShareAt` already ramps it out, so this over the top of them would be the
    /// content's own sunset counted twice.
    ///
    /// @param upward the `z` of a unit direction. At or below nothing gives the horizon's own figure.
    osg::Vec3f airTransmittance(float upward);

    /// A colour as the content files store one, decoded.
    ///
    /// Morrowind's colours are display-encoded, and the light transport downstream is linear. The
    /// two differ most in the middle, so mid grey is where a renderer that skips this is most
    /// obviously wrong and where a test pins it.
    osg::Vec3f decodeColour(std::uint32_t packed);

    /// The same decode, for a colour something else has already unpacked to `[0, 1]`.
    ///
    /// **What the game hands over is display-encoded too.** OpenMW's own renderer works in that
    /// space from end to end and never converts, so every colour read off a light, a fog or the sky
    /// is the file's own number divided by 255 — and a ray tracer that took it as linear would be
    /// as wrong there as it would be reading the record itself. The alpha is dropped: nothing
    /// downstream has a use for it.
    osg::Vec3f decodeColour(const osg::Vec4f& encoded);

    /// The same decode again, for a colour that never had an alpha — `Sky::sunDiscAt`'s is one.
    osg::Vec3f decodeColour(const osg::Vec3f& encoded);
}
