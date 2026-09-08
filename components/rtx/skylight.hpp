#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include <osg/Vec3f>

#include <components/esm3/loadcell.hpp>
#include <components/misc/constants.hpp>
#include <components/sky/timeofday.hpp>

#include "fogbuilder.hpp"
#include "sun.hpp"

namespace Rtx
{
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
    ///
    /// **The whole of what lights a cell, and the unit a host picks between.** A room takes its own
    /// out of an `AMBI` record and everything under a sky takes `makeSkylight`'s, so a reading is a
    /// choice between two of these rather than an assembly of either.
    struct Skylight
    {
        Sun mSun;

        /// The same sun as a layer above the ground sees it, out of `SkyReading::mSunShareAloft`.
        Sun mSunAloft;

        /// What a path is terminated with, which is the weather's own ambient plus whatever of the
        /// sun is not over the horizon. `makeSkylight` says why.
        osg::Vec3f mAmbient;

        /// What to hold a measured exposure back by for this light. One leaves it alone.
        ///
        /// **A field and not `exposureBias` of the two above, because a room breaks that
        /// derivation.** An interior's ambient is the whole of its light and the record is dark by
        /// the same measure a midnight is — so the function would hold a cellar back by two stops,
        /// which is not what an eye walking into one does. `makeRoomLight` states one instead.
        float mExposureBias = 1.0f;
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
    /// and nothing is lost. A night simply stops having a sun in it.
    Skylight makeSkylight(const SkyReading& sky);

    /// What to hold a measured exposure back by, for a sky delivering this much light.
    ///
    /// One where the hour delivers a full sun's worth or more, falling from there.
    ///
    /// **Night is a thing the world knows and not a thing the picture can measure.** A histogram has
    /// no absolute anchor: it normalises whatever it is shown toward the key, so a midnight and a
    /// noon come out within a few per cent of each other and the renderer has no night in it at any
    /// hour. The weather does know the hour, so it says how dark the hour is and the exposure pass
    /// is told rather than left to guess.
    float exposureBias(const osg::Vec3f& sunIrradiance, const osg::Vec3f& ambient);

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

    /// The sun, the sky and the air of one cell, in the renderer's own units.
    ///
    /// **Two things build one**, and neither can do the other's: `makeRoomLight` out of an
    /// interior's `AMBI` record, and a host out of what its weather system settled on. What is here
    /// is already converted — colours linear, the fog an extinction — so nothing downstream reads a
    /// content file again.
    struct Daylight
    {
        /// What the sky lights with, whole.
        ///
        /// **Held rather than restated**, so a field added to a light reaches an hour's sky without
        /// anyone carrying it across.
        Skylight mLight;

        /// Sky radiance, linear, at the horizon and overhead. The horizon is the weather's fog
        /// colour, which is also the air the cloud deck hangs in and is lifted off.
        osg::Vec3f mSkyHorizon;
        osg::Vec3f mSkyZenith;

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
    /// **One table, because a name and an index are asked for in different places.** A run names the
    /// weather it wants to stand under and the renderer is handed the script id the weather system
    /// settled on, and the two have to mean the same sky.
    std::optional<std::uint32_t> weatherIndex(std::string_view weather);

    /// The name that index spells, for whoever has to hand one back. Empty for an index past the
    /// ten.
    std::string_view weatherName(std::uint32_t weather);

    /// A room's light, out of its own `AMBI` record — the one place a `Daylight` is built without
    /// a sky, and what every interior is lit by.
    ///
    /// **The record, and not the rasterizer's reading of it.** `RenderingManager::configureAmbient`
    /// lifts an interior's ambient to `minimum interior brightness` before its own lights see it,
    /// which balances a falloff curve this renderer does not have, and `openmw-rtxtool` has no
    /// rasterizer to read. So both hand over the four numbers the cell wrote, in the record's own
    /// type, and this is the one reading of them.
    ///
    /// **A room has no sun, and its sunlight is spread instead.** `configureAmbient` puts the
    /// record's sunlight into a directional light aimed along `(-1, 45°, 45°)` — two angles in
    /// radians used as coordinates, which upstream's own comment calls nonsense and uses anyway. A
    /// rasterizer can afford an arbitrary direction because the light it casts is soft and stops at
    /// the geometry it is given. Traced, that direction is real: it casts hard shadows off nothing,
    /// and it reaches through every crack a room's shell is built from, which is a bright seam
    /// wherever two walls meet.
    ///
    /// So the record's sunlight is kept whole and its direction is taken away, over `INV_FOUR_PI` —
    /// the same move `makeSkylight` makes on the night's sun, for the same reason. A room is as
    /// bright as its record says and no longer has a direction the content never chose.
    ///
    /// The sky is the fog colour at both ends, since a room has no dome and its air stands in
    /// wherever a ray gets out. **It is a colour and never a light**: nothing outside a room lights
    /// anything in it, and `describeWorld` is where that is said once for both hosts. The stars are
    /// nought and the exposure bias is one, which is what the game holds a room at.
    ///
    /// @param nightEye what the Night-Eye effect adds to every channel of the ambient, in the
    ///        file's own space — which is where `RenderingManager::updateAmbient` adds it, so it is
    ///        added before the decode here as well. Nothing for a caller with no spell in force.
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
}
