#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include <osg/Vec3f>

#include <components/esm3/loadcell.hpp>
#include <components/misc/constants.hpp>
#include <components/sky/timeofday.hpp>

#include "fogbuilder.hpp"

namespace Rtx
{
    /// The sun, as a directional light and as something to look at. Far enough away that its rays
    /// are parallel, so it has a direction and no position.
    ///
    /// Nothing may fill these fields itself: `makeSkylight` builds every one of them for the world,
    /// and `OffscreenTrace::setLight` for a picture inside the interface lit by a flat light with no
    /// hour behind it. Here and not in `SceneDesc`, because a sun is what the frame's *world* is
    /// doing.
    struct Sun
    {
        /// Where the sun stands, unit — and so `-mPosition` is where its light travels.
        /// `MWRender::WorldState::mSunPosition` says why it is kept through a night that has no sun.
        osg::Vec3f mPosition{ 0.0f, 0.0f, 1.0f };

        /// Irradiance on a surface square to it, linear. Zero exactly when there is no sun, which
        /// is the invariant the whole type exists for: everything the sun does is gated on this one
        /// test, so a sun cannot shadow without being drawn or be drawn without lighting.
        osg::Vec3f mIrradiance;

        /// What the disc is painted with, linear — not the hue of `mIrradiance`, which is the sky's
        /// as much as the sun's and blue at night. `MWWorld::WeatherManager::calculateResult` is
        /// where it comes from, with the weather's glare folded in.
        osg::Vec3f mDiscColour{ 1.0f, 1.0f, 1.0f };
    };

    /// What a weather says about the sky at one hour, in the renderer's own units. Both hosts reach
    /// these by their own route — the game reads a live weather system, the harness derives them
    /// from the content files — and hand them to `makeSkylight` rather than assembling a sun.
    struct SkyReading
    {
        /// Where the disc stands, unit — `MWRender::WorldState::mSunPosition`. The field and not the
        /// struct, which carries a light direction and a night switch that are the rasterizer's own
        /// and that neither host has to give.
        osg::Vec3f mSunPosition = osg::Vec3f(0.0f, 0.0f, 1.0f);

        /// How much of the sun is over the horizon — `Sky::sunShareAt`, which alone answers "is
        /// there a sun".
        float mSunShare = 0.0f;

        /// How much of it a layer standing above the ground still has — `sunShareAloft`. A cloud
        /// deck keeps the sun after the ground has lost it, at the same place and colour. Nought
        /// where nothing stands above the ground to ask.
        float mSunShareAloft = 0.0f;

        /// The weather's `Sun_*_Color` at this hour, linear — Morrowind's own, night blue and all.
        osg::Vec3f mSunColour;

        /// The weather's `Ambient_*_Color` at this hour, linear.
        osg::Vec3f mAmbient;

        /// What the disc is painted with, linear. `MWRender::WorldState::mSunDiscColour`.
        osg::Vec3f mDiscColour = osg::Vec3f(1.0f, 1.0f, 1.0f);

        /// The weather's `Glare_View`: how much of the sun it lets through.
        float mGlare = 1.0f;
    };

    /// The sky's light, in the two forms a tracer can use it: one that comes from somewhere, and one
    /// that does not. The whole of what lights a cell: a room takes its own out of an `AMBI` record
    /// and everything under a sky takes `makeSkylight`'s.
    struct Skylight
    {
        Sun mSun;

        /// The same sun as a layer above the ground sees it, out of `SkyReading::mSunShareAloft`.
        Sun mSunAloft;

        /// What a path is terminated with: the weather's own ambient plus whatever of the sun is not
        /// over the horizon. `makeSkylight` says why.
        osg::Vec3f mAmbient;

        /// What to hold a measured exposure back by for this light. One leaves it alone. A field
        /// and not `exposureBias` of the two above, because a room breaks that derivation: an
        /// interior's ambient is the whole of its light and the record is as dark as a midnight, so
        /// the function would hold a cellar back by two stops. `makeRoomLight` states one instead.
        float mExposureBias = 1.0f;
    };

    /// The sky's light, out of what a weather says — the one place a sun is allowed to be built.
    ///
    /// Morrowind never switches its sunlight off: `WeatherManager` reads a colour off the same ramp
    /// all night — `Sun_Night_Color` is `59, 97, 176` — and turns off only the *sprite*. A
    /// rasterized directional light with no visible source looks like nothing in particular; traced,
    /// it casts hard shadows that swing across the ground all night. So what the file calls the
    /// night's sun goes where light with no direction belongs — the ambient — and the sun keeps only
    /// what is over the horizon. The two halves are complements, so the total is continuous through
    /// dusk, which is also what twilight is.
    ///
    /// The share that goes into the ambient is a quarter of the irradiance over pi: a directional
    /// source delivers, averaged over every orientation, a quarter of its irradiance, and a uniform
    /// hemisphere of radiance `L` delivers `pi L`, so `E / 4pi` is the same light with the direction
    /// taken out.
    Skylight makeSkylight(const SkyReading& sky);

    /// What to hold a measured exposure back by, for a sky delivering this much light. One where
    /// the hour delivers a full sun's worth or more, falling from there.
    ///
    /// Night is a thing the world knows and not a thing the picture can measure: a histogram
    /// normalises whatever it is shown toward the key, so a midnight and a noon come out within a
    /// few per cent of each other. The weather knows the hour, so it says how dark the hour is.
    float exposureBias(const osg::Vec3f& sunIrradiance, const osg::Vec3f& ambient);

    /// How high the cloud layer stands, in world units — the one number in the sky that is chosen
    /// rather than read, because the cloud mesh gives its height in tiles of its own sheet and no
    /// metre anywhere. Five hundred metres is a stratocumulus base and the reference
    /// implementation's own choice. It settles what `sunShareAloft` reads the sun at and how large a
    /// shadow the deck casts. A world height and not a height over the eye: a layer that rose with
    /// the camera would cast a shadow that moved with it.
    inline constexpr float sCloudAltitude = 500.0f * Constants::UnitsPerMeter;

    /// How much of the sun a layer standing over the ground still has at `hour`.
    ///
    /// The engine's sunset is a clock and not a horizon: `Sky::sunShareAt` ramps on the hour and
    /// the weather manager puts the disc level with the horizon at exactly `mNightStart`, so a
    /// layer that keeps the sun past the ground's horizon is handed a different hour instead of a
    /// lower one. How far the clock moves is the dip a layer that high sees, over the time the disc
    /// takes to fall it: 0.718 degrees at 5.35 game minutes on the shipped fourteen-hour day, a
    /// shift of 4.5% against the two-hour dusk, and 8.7% of the sun still held at the instant the
    /// ground's goes out. Its day is the ground's widened at both ends rather than moved. Nothing is
    /// done to the colour, because `Sun_Sunset_Color` is the content's own reddening keyed on the
    /// same hour.
    float sunShareAloft(float hour, const Sky::TimeOfDaySettings& times);

    /// What the sky delivers to a surface facing it, and how much of that it is never drawn with.
    struct SkyBudget
    {
        /// The whole of it, as a radiance: the gradient, the night's sheets and `mFill` together.
        /// What lights anything the sky stands over, which so far is the cloud deck.
        osg::Vec3f mMean;

        /// What the sky delivers as light over and above the colour it is drawn with.
        ///
        /// Morrowind lights its night with an ambient and this renderer lights it with a sky, and
        /// the two are an order apart: the engine puts `Ambient_<weather>_Night_Color` on every
        /// surface directly, while a bounce ray at the dome reads `Sky_<weather>_Night_Color`, which
        /// is a tenth of it. So the sky is held to what the weather says a night is worth: a
        /// gradient linear in `sin(elevation)` delivers what a uniform sky of `horizon / 3 + 2 *
        /// zenith / 3` would, the night's sheets add their mean, and whatever the ambient asks for
        /// beyond the two is this. It is light and not a colour — nothing draws it, because
        /// Morrowind does not draw it either — and every layer that lights comes out of the same
        /// figure, so a night does not brighten each time one more of them starts lighting. Nought
        /// by day, because a weather's daylight sky outruns its daylight ambient in every channel.
        osg::Vec3f mFill;
    };

    /// Reads both off one weather, so nothing can hold two ideas of what a sky is worth.
    ///
    /// @param sheets what the night sky's own layers add — `Shaders::StarField::mGlow`.
    SkyBudget skyBudget(
        const osg::Vec3f& horizon, const osg::Vec3f& zenith, const osg::Vec3f& sheets, const osg::Vec3f& ambient);

    /// The sun, the sky and the air of one cell, in the renderer's own units — colours linear, the
    /// fog an extinction — so nothing downstream reads a content file again. Built by
    /// `makeRoomLight` out of an interior's `AMBI` record, or by a host out of its weather.
    struct Daylight
    {
        /// What the sky lights with, whole. Held rather than restated, so a field added to a light
        /// reaches an hour's sky without anyone carrying it across.
        Skylight mLight;

        /// Sky radiance, linear, at the horizon and overhead. The horizon is the weather's fog
        /// colour, which is also the air the cloud deck hangs in.
        osg::Vec3f mSkyHorizon;
        osg::Vec3f mSkyZenith;

        /// How far the stars have come out: the engine's `Stars` ramp at this hour, before the
        /// weather's glare is taken off it.
        float mStarFade = 0.0f;

        /// The weather's own air. Its colour is `mSkyHorizon`, because Morrowind records one colour
        /// for the fog and for the sky's lower half — the horizon *is* fog — so a ray that reaches
        /// nothing and a ray through a mile of air arrive at the same answer.
        Fog mFog;
    };

    /// A weather's index, as `MWWorld::WeatherManager` registers them and the shader's `WEATHER_*`
    /// name them, or nothing for a name that is none of the ten. One table, because a run names
    /// the weather it wants and the renderer is handed the script id the weather system settled on.
    std::optional<std::uint32_t> weatherIndex(std::string_view weather);

    /// The name that index spells. Empty for an index past the ten.
    std::string_view weatherName(std::uint32_t weather);

    /// A room's light, out of its own `AMBI` record — the one place a `Daylight` is built without
    /// a sky, and what every interior is lit by.
    ///
    /// The record, and not the rasterizer's reading of it: `RenderingManager::configureAmbient`
    /// lifts an interior's ambient to `minimum interior brightness`, which balances a falloff curve
    /// this renderer does not have, and `openmw-rtxtool` has no rasterizer to read.
    ///
    /// A room has no sun, and its sunlight is spread instead. `configureAmbient` aims the record's
    /// sunlight along `(-1, 45°, 45°)` — two angles in radians used as coordinates, which upstream's
    /// own comment calls nonsense. Traced, that direction is real: hard shadows off nothing, and a
    /// bright seam through every crack a room's shell is built from. So the sunlight is kept whole
    /// and its direction taken away, over `INV_FOUR_PI`, as `makeSkylight` does with the night's
    /// sun. The sky is the fog colour at both ends, a colour and never a light: nothing outside a
    /// room lights anything in it. The stars are nought and the exposure bias is one.
    ///
    /// @param nightEye what the Night-Eye effect adds to every channel of the ambient, in the
    ///        file's own space — where `RenderingManager::updateAmbient` adds it, so before the
    ///        decode here as well. Nothing for a caller with no spell in force.
    Daylight makeRoomLight(const ESM::Cell::AMBIstruct& room, const osg::Vec3f& nightEye = osg::Vec3f());

    /// What the air leaves of a body in the sky, per channel — the reason a moon can rise at all
    /// here. The engine draws no moon under `Moons_<name>_Fade_End_Angle` because a lit quad over
    /// its own fogged dome reads as a sticker; a renderer that traces the air lets the air take a
    /// low moon out on its own.
    ///
    /// Rayleigh optical depth at the three sRGB primaries, times the air mass along the slant path.
    /// The depth is `0.008569 λ^-4` with its usual correction — 0.068, 0.097 and 0.221 at the zenith
    /// — and the air mass is Kasten and Young's fit, 37.92 at the horizon against one overhead. So
    /// a moon comes up a deep red ember and is itself by thirty degrees. Not applied to the sun,
    /// whose `Sun_Disc_Sunset_Color` and `sunShareAt` are the content's own sunset already.
    ///
    /// @param upward the `z` of a unit direction. At or below nothing gives the horizon's own figure.
    osg::Vec3f airTransmittance(float upward);
}
