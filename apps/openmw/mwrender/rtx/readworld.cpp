#include "readworld.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <components/esm3/loadcell.hpp>
#include <components/rtx/fogbuilder.hpp>
#include <components/rtx/lightbuilder.hpp>
#include <components/rtx/moonbuilder.hpp>
#include <components/sky/timeofday.hpp>

#include "../sceneframe.hpp"

namespace MWRender
{
    WorldRead readWorld(const WorldState& world, const Rtx::SkyContent& sky, const Rtx::MoonFaces& faces,
        const float landReach, const float seconds)
    {
        // **Decoded here, because the world does not know what a transport is.** Every colour on
        // the frame is a content file's three bytes over 255 and no transfer function; the
        // rasterizer samples them as they are and this light transport is linear, so the conversion
        // belongs to whichever renderer needs it.
        // **Where the sun *is*, and the light comes back along it.** The world also reports
        // `mSunVector`, which is where the rasterizer's light travels and is not the negation of
        // this — `Sky::sunAt` says why, and why nothing that traces can hold both.
        osg::Vec3f discAt(world.mSunPosition.x(), world.mSunPosition.y(), world.mSunPosition.z());
        if (discAt.length2() > 0.0f)
            discAt.normalize();

        // The horizon is the fog and the zenith is the sky's own, which is the pair Morrowind
        // records: one colour for the air, and one for the dome it fades into overhead.
        // **A room's light is built once, out of the record the cell wrote** — `Rtx::makeRoomLight`,
        // which is what `openmw-rtxtool` reads out of the content files — and not out of the
        // rasterizer's reading of it. `mAmbientColour` carries the lift `configureAmbient` gives an
        // interior for its own falloff curve and `mSunPosition` where it points a directional
        // light, and neither is a fact about the room. What the game adds for Night-Eye comes over
        // as itself.
        const std::optional<Rtx::Daylight> room = world.isOutdoors()
            ? std::nullopt
            : std::optional(Rtx::makeRoomLight(ESM::Cell::AMBIstruct{ .mAmbient = world.mRoomAmbient,
                                                   .mSunlight = world.mRoomSunlight,
                                                   .mFog = world.mRoomFog,
                                                   .mFogDensity = world.mFogDepth },
                osg::Vec3f(world.mNightEye.x(), world.mNightEye.y(), world.mNightEye.z())));

        const osg::Vec3f haze = room.has_value() ? room->mSkyHorizon : Rtx::decodeColour(world.mAir.mColour);

        // **The sky's own colour, and an interior has none.** The weather system stops writing it
        // the moment the player steps inside, so what the sky is still holding belongs to wherever
        // they were last outdoors — and the air's own colour stands in, which is what a room's sky
        // is anyway. A quasi-exterior is on the outdoor side of that: it has weather.
        const osg::Vec3f zenith = room.has_value() ? room->mSkyZenith : Rtx::decodeColour(world.mSkyColour);

        // **The sun is not assembled here.** Everything the world says about it goes to the one
        // builder that decides what a sun may be — which is what keeps the game and the harness
        // under the same sky, and what makes a sun that lights an empty night impossible to write.
        // A room has none, and `Rtx::makeRoomLight` is where that is said for both hosts.
        const Rtx::Skylight daylight = room.has_value()
            ? Rtx::Skylight{ .mSun = room->mSun, .mSunAloft = room->mSunAloft, .mAmbient = room->mAmbient }
            : Rtx::makeSkylight(Rtx::SkyReading{
                .mSunPosition = discAt,
                .mSunShare = world.mSunDiscColour.a(),

                // **The deck keeps the sun after the ground has lost it**, and the hour is what says how
                // much of it is left — `Rtx::sunShareAloft`. The ground's own share arrives from the
                // weather system, which reads the same `Sky::sunShareAt` at the same hour.
                .mSunShareAloft = Rtx::sunShareAloft(world.mGameHour, Sky::TimeOfDaySettings::shared()),
                .mSunColour = Rtx::decodeColour(world.mSunColour),
                .mAmbient = Rtx::decodeColour(world.mAmbientColour),
                .mDiscColour = Rtx::decodeColour(world.mSunDiscColour),
                .mGlare = world.mSunGlare,
            });

        // **The recorded depth, and not the ramp `MWRender::FogManager` made of it.** That ramp
        // exists to hide a far clip plane and this renderer has no far clip to hide, so per the
        // fork's own rule it does not come across. What is read instead is the number the content
        // wrote, handed to the one builder that knows what a cell's air may be — which is what
        // `openmw-rtxtool` calls out of the same records, so a screenshot and a played frame stand
        // in one air.
        //
        // **A quasi-exterior takes a room's air under an outdoor sky**, and this is the one place
        // that can tell one: `WorldReading::mFogFromSky` says what turns on it.
        const bool fogFromSky = world.isOutdoors() && !world.isInteriorCell();
        const Rtx::Fog air = room.has_value() ? room->mFog
            : fogFromSky                      ? Rtx::exteriorFog(haze, world.mFogDepth, world.mBaseWindSpeed, landReach)
                                              : Rtx::roomFog(haze, world.mFogDepth);

        // **Before the frame rather than into it, because the deck is lit by them.** A cloud layer
        // takes the moons' light like anything else under a night sky, and `Rtx::deckLight` is
        // handed the pair.
        std::array<Rtx::MoonPlacement, 2> moons{};
        for (std::size_t moon = 0; moon < moons.size(); ++moon)
        {
            const MoonState& state = world.mMoons[moon];

            // `Unspecified` is a ninth value and not a phase; the weather system uses it to mean it
            // has not spoken, and a moon it has not spoken about is one with no alpha anyway.
            const int phase = state.mPhase == MoonState::Phase::Unspecified ? 0 : static_cast<int>(state.mPhase);

            // **The glare is applied here and not by the weather system**, which is where the
            // rasterizer applies it too: `SkyManager::setWeather` calls `Moon::adjustTransparency`
            // with it after the state has been handed over. A thunderstorm hides its moons the same
            // way it hides its stars.
            moons[moon] = Rtx::placeMoon(static_cast<Rtx::Moon>(moon), state.mRotationFromHorizon,
                state.mRotationFromNorth, phase, state.mDaylightFade * world.mSunGlare);
            moons[moon].mFace = faces.of(static_cast<Rtx::Moon>(moon));
        }

        const auto weatherId = static_cast<std::uint32_t>(world.mWeatherId);

        return WorldRead{ .mReading = Rtx::WorldReading{
            .mDaylight = Rtx::Daylight{
                .mSun = daylight.mSun,
                .mSunAloft = daylight.mSunAloft,
                .mSkyHorizon = haze,
                .mSkyZenith = zenith,
                .mAmbient = daylight.mAmbient,
                .mStarFade = world.mNightFade,
                .mFog = air,
            },
            .mOutdoors = world.isOutdoors(),
            .mFogFromSky = fogFromSky,
            .mGlare = world.mSunGlare,
            .mStarRoll = world.mSkyRoll.mStars,
            .mCloudRoll = world.mSkyRoll.mClouds,
            .mSky = sky,
            .mMoons = moons,
            .mWeather = weatherId,

            // **The current weather twice where nothing is arriving**, since the deck crosses
            // unconditionally: naming it on both sides at a blend of nothing is what lets it.
            .mNextWeather = world.mNextWeatherId.has_value() ? static_cast<std::uint32_t>(*world.mNextWeatherId)
                                                             : weatherId,
            .mCloudBlend = world.mCloudBlend,
            .mCloudDirection = world.mCloudDirection,
            .mNextCloudDirection = world.mNextCloudDirection,

            // Negative infinity and not zero: zero is sea level, and a cell with no water has to
            // answer "how deep is this point" with never.
            .mWaterLevel = world.mWaterEnabled ? world.mWaterHeight : -std::numeric_limits<float>::infinity(),

            // **What the sea is animated by, and leaving it at zero is a frozen ocean.** Real
            // elapsed seconds rather than the frame count: a sea that ran at the frame rate would
            // slow down whenever the frame did.
            .mSeconds = seconds,
            .mRainOnWater = Rtx::rainOnWater(world.mPrecipitation),
        },
            .mExposureBias = room.has_value() ? std::optional(room->mExposureBias) : std::nullopt };
    }
}
