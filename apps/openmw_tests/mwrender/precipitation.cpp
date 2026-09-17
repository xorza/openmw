#include <gtest/gtest.h>

#include <osg/Camera>
#include <osg/Group>
#include <osg/ref_ptr>

#include <components/resource/bgsmfilemanager.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/resource/niffilemanager.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/vfs/manager.hpp>

#include "apps/openmw/mwrender/precipitation.hpp"
#include "apps/openmw/mwrender/skyutil.hpp"

namespace MWRender
{
    namespace
    {
        /// A weather that rains, with the numbers `Precipitation::createRain` sizes the box by and
        /// divides by. Morrowind's own for Rain.
        WeatherResult rain()
        {
            WeatherResult weather{};
            weather.mRainEffect = "meshes/raindrop.nif";
            weather.mRainDiameter = 600.0f;
            weather.mRainMinHeight = 200.0f;
            weather.mRainMaxHeight = 700.0f;
            weather.mRainSpeed = 200.0f;
            weather.mRainEntranceSpeed = 1.0f;
            weather.mRainMaxRaindrops = 650;
            weather.mPrecipitationAlpha = 1.0f;
            return weather;
        }

        /// **Nothing falls while the sky is off.** The weather manager turns the sky off the moment
        /// the player steps indoors and stops writing the weather, so the rain box and the driven
        /// effect stay built under a root whose mask hides them from the rasterizer's cull. A walk
        /// that starts at the node never meets that mask: handed the nodes, the ray tracer drew the
        /// rain in every room entered from one.
        TEST(RtxPrecipitationTest, nothingFallsWhileTheSkyIsOff)
        {
            VFS::Manager vfs;
            Resource::ImageManager images(&vfs, 0);
            Resource::NifFileManager nifs(&vfs, nullptr);
            Resource::BgsmFileManager materials(&vfs, 0);
            Resource::SceneManager scenes(&vfs, &images, &nifs, &materials, 0);

            // The box's shaders are the rasterizer's, and an empty archive holds none of them.
            scenes.setShadersEnabled(false);

            osg::ref_ptr<osg::Group> root = new osg::Group;
            osg::ref_ptr<osg::Camera> camera = new osg::Camera;
            Precipitation precipitation(root, camera, &scenes);

            precipitation.setWeather(rain());
            ASSERT_NE(precipitation.getRainNode(), nullptr) << "a weather that rains makes the box";
            EXPECT_EQ(precipitation.getParticleNode(), nullptr) << "and drives nothing else";

            precipitation.setEnabled(false);
            EXPECT_EQ(precipitation.getRainNode(), nullptr);
            EXPECT_EQ(precipitation.getParticleNode(), nullptr);
            EXPECT_TRUE(precipitation.hasRain()) << "the box itself stays built, as the rasterizer keeps it";

            precipitation.setEnabled(true);
            EXPECT_NE(precipitation.getRainNode(), nullptr) << "and the same box is back outdoors";
        }
    }
}
