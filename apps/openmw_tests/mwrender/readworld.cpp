#include <gtest/gtest.h>

#include <osg/Vec3f>
#include <osg/Vec4f>

#include <components/misc/constants.hpp>
#include <components/rtx/fogbuilder.hpp>
#include <components/rtx/moonbuilder.hpp>
#include <components/rtx/skybuilder.hpp>

#include "apps/openmw/mwrender/rtx/readworld.hpp"
#include "apps/openmw/mwrender/sceneframe.hpp"

namespace MWRender
{
    namespace
    {
        constexpr float sReach = 4.0f * static_cast<float>(Constants::CellSizeInUnits);

        /// Noon under clear weather, wherever the caller says the player is standing.
        WorldState standingIn(const Location where)
        {
            WorldState world;
            world.mLocation = where;
            world.mGameHour = 12.0f;
            world.mFogDepth = 0.69f;
            world.mBaseWindSpeed = 0.3f;
            world.mAir = { .mColour = osg::Vec4f(0.62f, 0.77f, 1.0f, 1.0f) };
            world.mSkyColour = osg::Vec4f(0.11f, 0.24f, 0.6f, 1.0f);
            world.mSunPosition = osg::Vec4f(0.0f, 0.0f, 1.0f, 0.0f);
            world.mSunColour = osg::Vec4f(1.0f, 0.97f, 0.85f, 1.0f);
            world.mSunDiscColour = osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f);

            return world;
        }

        Rtx::WorldReading readFrom(const WorldState& world)
        {
            return readWorld(world, Rtx::SkyContent{}, Rtx::MoonFaces{}, sReach, 0.0f);
        }

        /// A quasi-exterior stands under the exterior's sun and in the exterior's air.
        ///
        /// **Because that is the cell the game hands it.** `World::updateWeather` runs the weather
        /// system for `isCellExterior() || isCellQuasiExterior()`, so what reaches a reading of
        /// Mournhold or of a Vivec canton is a weather's own `Land_Fog_Depth`, fog colour and sun —
        /// and the Construction Set greys the whole `AMBI` record out for such a cell, so there is
        /// nothing of a room's to read instead.
        ///
        /// **Read as a room's air it lost the sky.** `roomFog` builds an even medium with no layer
        /// for a ray to climb out of, so a ray to the sun crossed every unit of it: Mournhold at
        /// noon came back as flat fog colour with no sun anywhere in it.
        TEST(RtxReadWorldTest, aQuasiExteriorTakesTheWeathersSunAndAir)
        {
            const Rtx::WorldReading open = readFrom(standingIn(Location::Exterior));
            const Rtx::WorldReading quasi = readFrom(standingIn(Location::QuasiExterior));

            EXPECT_TRUE(open.mOutdoors);
            EXPECT_TRUE(quasi.mOutdoors) << "a cell with weather over it is outdoors";

            // The sun the symptom was about: there is one at noon, and it is the exterior's.
            EXPECT_NE(quasi.mDaylight.mLight.mSun.mIrradiance, osg::Vec3f());
            EXPECT_EQ(quasi.mDaylight.mLight.mSun.mIrradiance, open.mDaylight.mLight.mSun.mIrradiance);
            EXPECT_EQ(quasi.mDaylight.mLight.mSun.mPosition, open.mDaylight.mLight.mSun.mPosition);
            EXPECT_EQ(quasi.mDaylight.mLight.mAmbient, open.mDaylight.mLight.mAmbient);

            // And the sky it hangs in, which an air read as a room's had closed over.
            EXPECT_EQ(quasi.mDaylight.mSkyZenith, open.mDaylight.mSkyZenith);
            EXPECT_EQ(quasi.mDaylight.mSkyHorizon, open.mDaylight.mSkyHorizon);

            const Rtx::Fog& air = quasi.mDaylight.mFog;
            EXPECT_EQ(air.mColour, open.mDaylight.mFog.mColour);
            EXPECT_EQ(air.mExtinction, open.mDaylight.mFog.mExtinction);
            EXPECT_EQ(air.mUniform, 0.0f) << "banked, as every other weather's air is";
            EXPECT_EQ(air.mLift, open.mDaylight.mFog.mLift) << "and standing as high";
            EXPECT_EQ(air.mWind, open.mDaylight.mFog.mWind);

            // **The one element that parts them**, because it is about this renderer and not about
            // the weather: the ring where the ground stops is what it hides, and every wall of a
            // quasi-exterior is built.
            EXPECT_EQ(open.mDaylight.mFog.mEdge, sReach);
            EXPECT_EQ(air.mEdge, 0.0f);
        }

        /// A room is lit by its own record, and the record is the only thing that decides it.
        ///
        /// **The alternative the two above are not.** A cell that is a room carries an `AMBI`, has
        /// no sun at any hour, and holds the still even air `sInteriorFogReach` is measured over.
        TEST(RtxReadWorldTest, aRoomIsLitByItsOwnRecordAndHasNoSun)
        {
            WorldState cellar = standingIn(Location::Interior);
            cellar.mRoom = RoomMood{ .mAmbient = 0x00201818u, .mSunlight = 0x00403028u, .mFog = 0x00151510u };

            const Rtx::WorldReading room = readFrom(cellar);

            EXPECT_FALSE(room.mOutdoors);
            EXPECT_EQ(room.mDaylight.mLight.mSun.mIrradiance, osg::Vec3f()) << "noon reached a cellar";
            EXPECT_EQ(room.mDaylight.mFog.mUniform, 1.0f);
            EXPECT_EQ(room.mDaylight.mFog.mEdge, 0.0f);
            EXPECT_NEAR(
                room.mDaylight.mFog.mExtinction, Rtx::fogExtinction(cellar.mFogDepth, Rtx::sInteriorFogReach), 1e-10f);

            // Its sky is its own air and not the one the player last stood under, which the weather
            // system stopped writing the moment they stepped inside.
            EXPECT_EQ(room.mDaylight.mSkyZenith, room.mDaylight.mSkyHorizon);
            EXPECT_NE(room.mDaylight.mSkyZenith, readFrom(standingIn(Location::Exterior)).mDaylight.mSkyZenith);
        }
    }
}
