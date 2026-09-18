#include <cstddef>

#include <gtest/gtest.h>

#include <osg/Camera>
#include <osg/Group>
#include <osg/Vec3f>
#include <osg/Vec4f>
#include <osg/ref_ptr>

#include <components/esm3/loadcell.hpp>
#include <components/fallback/fallback.hpp>
#include <components/misc/constants.hpp>
#include <components/resource/bgsmfilemanager.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/resource/niffilemanager.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/rtx/colour.hpp>
#include <components/rtx/fogbuilder.hpp>
#include <components/rtx/frameworld.hpp>
#include <components/rtx/moonbuilder.hpp>
#include <components/rtx/skybuilder.hpp>
#include <components/sky/skyclock.hpp>
#include <components/vfs/manager.hpp>

#include "apps/openmw/mwrender/precipitation.hpp"
#include "apps/openmw/mwrender/rtx/skyreader.hpp"
#include "apps/openmw/mwrender/sceneframe.hpp"
#include "apps/openmw/mwrender/skystate.hpp"

namespace MWRender
{
    namespace
    {
        constexpr float sReach = 4.0f * static_cast<float>(Constants::CellSizeInUnits);

        /// The two records a reading is made of.
        struct Standing
        {
            SkyState mSky;
            WorldState mWorld;
        };

        /// Noon under clear weather, wherever the caller says the player is standing: the sun
        /// straight overhead, the weather run wherever there is a sky over the player.
        Standing standingIn(const Location where)
        {
            Standing standing;
            SkyState& sky = standing.mSky;
            WorldState& world = standing.mWorld;

            // Morrowind's shipped day: sunrise at six for two hours, sunset at eighteen for two.
            sky.mTimes.mNightEnd = 6.f;
            sky.mTimes.mSunriseDuration = 2.f;
            sky.mTimes.mDayStart = 8.f;
            sky.mTimes.mDayEnd = 18.f;
            sky.mTimes.mNightStart = 20.f;

            world.mLocation = where;
            world.mSkyShown = where != Location::Interior;
            sky.mOutdoors = where != Location::Interior;
            world.mGameHour = 12.0f;
            sky.mWeather.mFogDepth = 0.69f;
            sky.mWeather.mBaseWindSpeed = 0.3f;
            sky.mWeather.mGlareView = 1.0f;
            world.mAir = { .mColour = osg::Vec4f(0.62f, 0.77f, 1.0f, 1.0f) };
            sky.mWeather.mSkyColor = osg::Vec4f(0.11f, 0.24f, 0.6f, 1.0f);

            // An orbit direction whose disc, bent by `Sky::sunDiscPosition`, stands straight up;
            // and the same point on the light, for a room where the weather has not run.
            sky.mSunDirection = osg::Vec3f(0.0f, 0.0f, -100.0f);
            world.mSunLightPosition = osg::Vec4f(0.0f, 0.0f, 1.0f, 0.0f);
            world.mSunColour = osg::Vec4f(1.0f, 0.97f, 0.85f, 1.0f);
            sky.mWeather.mSunDiscColor = osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f);

            return standing;
        }

        /// What the weather drops, over an empty archive: nothing, until a test hands it a
        /// weather that rains.
        struct Falling
        {
            VFS::Manager mVfs;
            Resource::ImageManager mImages{ &mVfs, 0 };
            Resource::NifFileManager mNifs{ &mVfs, nullptr };
            Resource::BgsmFileManager mMaterials{ &mVfs, 0 };
            Resource::SceneManager mScenes{ &mVfs, &mImages, &mNifs, &mMaterials, 0 };
            osg::ref_ptr<osg::Group> mRoot = new osg::Group;
            osg::ref_ptr<osg::Camera> mCamera = new osg::Camera;
            Precipitation mPrecipitation{ mRoot, mCamera, &mScenes };

            Falling()
            {
                // The box's shaders are the rasterizer's, and an empty archive holds none of them.
                mScenes.setShadersEnabled(false);
            }
        };

        /// A reader at the reach the assertions below count on, with the sky and the moons it
        /// holds before a world arrives: none, and nothing falling.
        Rtx::WorldReading readFrom(const Standing& standing)
        {
            const SkyReader reader;
            const Falling falling;

            return reader.read(standing.mSky, standing.mWorld, falling.mPrecipitation, 0.0f, sReach);
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

        /// The disc stands where the orbit's direction puts it, bent toward the horizon by the
        /// rule both renderers share, and not where the light happens to point: `match sunlight
        /// to sun` may have left the light on the orbit, and the shadows have to fall from the disc.
        TEST(RtxReadWorldTest, theDiscIsWhereTheOrbitPutsItAndNotWhereTheLightPoints)
        {
            Standing dusk = standingIn(Location::Exterior);
            dusk.mSky.mSunDirection = osg::Vec3f(-300.0f, 75.0f, -100.0f);
            dusk.mWorld.mSunLightPosition = osg::Vec4f(300.0f, -75.0f, 100.0f, 0.0f);

            // `(300, -75, 400 - 300)`, unit length: the disc a hundred up where the light is
            // a hundred up too, but the disc's is the bent height and the light's the orbit's.
            osg::Vec3f disc(300.0f, -75.0f, 100.0f);
            disc.normalize();
            EXPECT_EQ(readFrom(dusk).mDaylight.mLight.mSun.mPosition, disc);

            dusk.mSky.mSunDirection = osg::Vec3f(-100.0f, 75.0f, -100.0f);
            osg::Vec3f higher(100.0f, -75.0f, 300.0f);
            higher.normalize();
            EXPECT_EQ(readFrom(dusk).mDaylight.mLight.mSun.mPosition, higher) << "the light did not move, the disc did";
        }

        /// The deck scrolls and the fog churns by the reader's own clock, which the frame steps
        /// and the frame does not carry: one frame at the shipped scale under the fastest deck
        /// moves the scroll and the seconds by that frame.
        TEST(RtxReadWorldTest, theDeckAndTheFogReadTheSkysClock)
        {
            constexpr float step = 1.0f / 60.0f;
            const Standing standing = standingIn(Location::Exterior);
            const Falling falling;

            SkyReader reader;
            const Rtx::WorldReading still
                = reader.read(standing.mSky, standing.mWorld, falling.mPrecipitation, 0.0f, sReach);
            EXPECT_EQ(still.mClouds.mScroll, 0.0f);
            EXPECT_EQ(still.mSkySeconds, 0.0);

            reader.step(step, Sky::sVanillaTimeScale, 400.0f);
            const Rtx::WorldReading stepped
                = reader.read(standing.mSky, standing.mWorld, falling.mPrecipitation, 0.0f, sReach);
            EXPECT_FLOAT_EQ(stepped.mClouds.mScroll, step);
            EXPECT_DOUBLE_EQ(stepped.mSkySeconds, static_cast<double>(step));
        }

        /// A room is lit by its own record, and the record is the only thing that decides it.
        ///
        /// **The alternative the two above are not.** A cell that is a room carries an `AMBI`, has
        /// no sun at any hour, and holds the still even air `sInteriorFogReach` is measured over.
        TEST(RtxReadWorldTest, aRoomIsLitByItsOwnRecordAndHasNoSun)
        {
            Standing cellar = standingIn(Location::Interior);
            cellar.mWorld.mRoom = ESM::Cell::AMBIstruct{ .mAmbient = 0x00201818u,
                .mSunlight = 0x00403028u,
                .mFog = 0x00151510u,
                .mFogDensity = cellar.mSky.mWeather.mFogDepth };

            const Rtx::WorldReading room = readFrom(cellar);

            EXPECT_FALSE(room.mOutdoors);
            EXPECT_EQ(room.mDaylight.mLight.mSun.mIrradiance, osg::Vec3f()) << "noon reached a cellar";
            EXPECT_EQ(room.mDaylight.mFog.mUniform, 1.0f);
            EXPECT_EQ(room.mDaylight.mFog.mEdge, 0.0f);
            EXPECT_NEAR(room.mDaylight.mFog.mExtinction,
                Rtx::fogExtinction(cellar.mSky.mWeather.mFogDepth, Rtx::sInteriorFogReach), 1e-10f);

            // Its sky is its own air and not the one the player last stood under, which the weather
            // system stopped writing the moment they stepped inside.
            EXPECT_EQ(room.mDaylight.mSkyZenith, room.mDaylight.mSkyHorizon);
            EXPECT_NE(room.mDaylight.mSkyZenith, readFrom(standingIn(Location::Exterior)).mDaylight.mSkyZenith);
        }

        /// `tsky` hides the sky and leaves the light: the rasterizer masks the sky node out and
        /// clears to the fog colour, and the sun goes on lighting the ground. So the reading is
        /// not outdoors — no deck, no stars, no moons, no dome fill — its zenith is its horizon,
        /// and its sun is the noon sun with no disc to draw.
        TEST(RtxReadWorldTest, theSkyToggleHidesTheSkyAndKeepsTheSun)
        {
            Standing standing = standingIn(Location::Exterior);
            standing.mWorld.mSkyShown = false;

            const Rtx::WorldReading hidden = readFrom(standing);
            const Rtx::WorldReading shown = readFrom(standingIn(Location::Exterior));

            EXPECT_FALSE(hidden.mOutdoors);
            EXPECT_EQ(hidden.mDaylight.mSkyZenith, hidden.mDaylight.mSkyHorizon);
            EXPECT_EQ(hidden.mDaylight.mSkyHorizon, shown.mDaylight.mSkyHorizon);
            EXPECT_EQ(hidden.mDaylight.mLight.mSun.mIrradiance, shown.mDaylight.mLight.mSun.mIrradiance);
            EXPECT_EQ(hidden.mDaylight.mLight.mSun.mDiscColour, osg::Vec3f()) << "a disc drawn on a hidden sky";
            EXPECT_NE(shown.mDaylight.mLight.mSun.mDiscColour, osg::Vec3f());
        }

        /// A script paints Secunda `Moons_Script_Color`, and Secunda alone, as
        /// `SkyManager::setMoonColour` paints it — read off the fallback map the mirror read once,
        /// which is what stands in for the game's own record here.
        TEST(RtxReadWorldTest, theScriptColourPaintsSecundaAlone)
        {
            const osg::Vec3f white(1.0f, 1.0f, 1.0f);
            const osg::Vec3f paint = Rtx::decodeColour(Fallback::Map::getColour("Moons_Script_Color"));
            ASSERT_NE(paint, white);

            Standing standing = standingIn(Location::Exterior);
            standing.mWorld.mMoonRed = true;

            const Rtx::WorldReading red = readFrom(standing);
            EXPECT_EQ(red.mMoons[static_cast<std::size_t>(Rtx::Moon::Masser)].mPaint, white);
            EXPECT_EQ(red.mMoons[static_cast<std::size_t>(Rtx::Moon::Secunda)].mPaint, paint);

            const Rtx::WorldReading plain = readFrom(standingIn(Location::Exterior));
            EXPECT_EQ(plain.mMoons[static_cast<std::size_t>(Rtx::Moon::Secunda)].mPaint, white);
        }

        /// What falls is kept off by a roof up to the top of the game's own occluder box — the
        /// precipitation's range and a cell over it — and by nothing where nothing falls. Asked of
        /// the precipitation itself: a weather that rains sizes the box by its own numbers, the
        /// height being the mean of the drops' two spawn heights.
        TEST(RtxReadWorldTest, theShelterIsTheOccludersBox)
        {
            const Standing standing = standingIn(Location::Exterior);
            const SkyReader reader;
            Falling falling;

            SkyState rain = standing.mSky;
            rain.mWeather.mRainEffect = "meshes/raindrop.nif";
            rain.mWeather.mRainDiameter = 600.0f;
            rain.mWeather.mRainMinHeight = 200.0f;
            rain.mWeather.mRainMaxHeight = 700.0f;
            rain.mWeather.mRainSpeed = 200.0f;
            rain.mWeather.mRainEntranceSpeed = 1.0f;
            rain.mWeather.mRainMaxRaindrops = 650;
            rain.mWeather.mPrecipitationAlpha = 1.0f;

            falling.mPrecipitation.setWeather(rain);
            EXPECT_EQ(reader.read(rain, standing.mWorld, falling.mPrecipitation, 0.0f, sReach).mShelterHeight,
                450.0f + static_cast<float>(Constants::CellSizeInUnits));

            falling.mPrecipitation.setWeather(standing.mSky);
            EXPECT_EQ(
                reader.read(standing.mSky, standing.mWorld, falling.mPrecipitation, 0.0f, sReach).mShelterHeight, 0.0f);
        }
    }
}
