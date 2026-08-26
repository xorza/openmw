#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include <osg/Vec3f>
#include <osg/Vec4f>

#include "fogbuilder.hpp"
#include "scenedesc.hpp"

namespace ESM
{
    struct Light;
    struct Region;
}

namespace Rtx
{
    /// The light a `LIGH` reference casts, or nothing where it casts none.
    ///
    /// Three kinds of record place no light. A **carried** one is a torch in a pack, lit only when
    /// something equips it. An **off by default** one does not burn while it sits in a cell. And a
    /// **negative** one *subtracts* illumination — a trick available to a renderer accumulating into
    /// a framebuffer and meaningless to anything that traces a ray to an emitter.
    std::optional<Light> makeLight(const ESM::Light& record, const osg::Vec3f& position);

    /// The same light, from a colour and a radius rather than from a record.
    ///
    /// **One conversion and two callers**, which is the point: the harness reads a cell's `LIGH`
    /// records and the game reads the `SceneUtil::LightSource` nodes its own scene graph already
    /// holds, and the two must not come to disagree about how bright a candle is.
    ///
    /// @param colour linear, as the game's own lighting already is.
    /// @param radius the recorded one. Null where it is not a size a light can have.
    std::optional<Light> makeLight(const osg::Vec3f& colour, float radius, const osg::Vec3f& position);

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
    /// that is still up lights as a direction, and the share that is not lights as a fill. That is
    /// also what twilight is.
    ///
    /// The fill is a quarter of the irradiance over pi. A directional source delivers, averaged over
    /// every orientation a surface could take, a quarter of its irradiance — the mean of `max(0,
    /// cos)` over the sphere — and a uniform hemisphere of radiance `L` delivers `pi L` to all of
    /// them, so `E / 4pi` is the same light with the direction taken out of it. Nothing is invented
    /// and nothing is lost; a night simply stops having a sun in it.
    Skylight makeSkylight(const SkyReading& sky);

    /// The sun and the sky at one hour, as the content files describe them.
    ///
    /// Every colour here is a fallback setting the game reads for itself, and the sun's arc, its
    /// four-point ramps and its disc all come from `components/sky` — the same arithmetic the
    /// weather manager runs, so a harness frame and a game frame stand under one sky rather than
    /// under two that were written to agree.
    struct Daylight
    {
        Sun mSun;

        /// Sky radiance, linear, at the horizon and overhead.
        osg::Vec3f mSkyHorizon;
        osg::Vec3f mSkyZenith;

        /// What an exterior gets in place of a cell's `AMBI`, which only interiors carry — and, at
        /// night, whatever the weather put in the sun's slot while the sun was not there.
        osg::Vec3f mAmbient;

        /// The weather's fog colour as the file records it, undecoded.
        ///
        /// **The one thing here that is not in the renderer's units**, and it is deliberate: the
        /// cloud deck is lit by this plus an eighth, added *before* the decode because that is where
        /// the engine adds it, so handing over the linear colour would lose the only form the lift
        /// is right in. `Sky::cloudColour` and `describeClouds` are the two halves of it.
        osg::Vec4f mHaze;

        /// How far the stars have come out: the engine's `Stars` ramp at this hour, before the
        /// weather's glare is taken off it.
        float mStarFade = 0.0f;

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
