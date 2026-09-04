#include "fixture.hpp"

#include <algorithm>
#include <span>

namespace Rtx::Testing
{
    namespace
    {
        /// What a white lamp of radius 100 radiates, once `makeLight` has derived it.
        ///
        /// Intensity is scaled by the square of the recorded radius, so `100 * 100 * 0.25 * pi` is
        /// 7853.98, and white decodes to one.
        constexpr float sWhiteLampAtHundred = 7853.98f;

        /// **What the walk asks a `LightSource` is what it radiates, and nothing else.**
        ///
        /// Two things it used to ask instead. `getEmpty` means the model this light hangs on has no
        /// geometry — a rasterizer's reason to skip drawing one, not a statement that the light is
        /// off — and a `LIGH` whose mesh is empty still burns. And the diffuse alone is not a light's
        /// colour: `Animation::setLightEffect` puts a glow light's whole colour in the ambient, so a
        /// Light spell read that way lit nothing at all.
        TEST(RtxSceneExtractorTest, aLightIsMirroredForWhatItRadiatesRatherThanForWhatItHangsOn)
        {
            osg::ref_ptr<SceneUtil::LightSource> lamp = makeLightSource(100.0f, osg::Vec4f(1, 1, 1, 1));

            // The flag the game sets on a light whose model draws nothing. It says nothing about
            // whether the light is lit.
            osg::ref_ptr<SceneUtil::LightSource> bare = makeLightSource(100.0f, osg::Vec4f(1, 1, 1, 1));
            bare->setEmpty(true);

            // A glow light: no diffuse at all, and 1.5 of ambient.
            osg::ref_ptr<SceneUtil::LightSource> glow
                = makeLightSource(100.0f, osg::Vec4f(0, 0, 0, 0), osg::Vec4f(1.5f, 1.5f, 1.5f, 1));

            osg::ref_ptr<osg::MatrixTransform> where
                = new osg::MatrixTransform(osg::Matrix::translate(10.0, 20.0, 30.0));
            where->addChild(lamp);
            where->addChild(bare);
            where->addChild(glow);

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            const ExtractionStats stats = extractor.extract(*where, osg::Matrixf::identity(), 0);

            EXPECT_EQ(stats.mLights, 3u);
            ASSERT_EQ(scene.getLights().size(), 3u);

            for (const Rtx::Light& light : scene.getLights())
                EXPECT_EQ(light.mPosition, osg::Vec3f(10.0f, 20.0f, 30.0f)) << "a light stood somewhere else";

            EXPECT_NEAR(scene.getLights()[0].mIntensity.x(), sWhiteLampAtHundred, 0.01f);
            EXPECT_EQ(scene.getLights()[1].mIntensity, scene.getLights()[0].mIntensity)
                << "an empty model dimmed the light hanging on it";

            // The same lamp scaled by what 1.5 of ambient decodes to.
            EXPECT_NEAR(scene.getLights()[2].mIntensity.x(), sWhiteLampAtHundred * 2.53716f, 0.05f);
        }

        /// A lamp the record says animates is mirrored at the instant the walk was told, not at rest.
        ///
        /// **Because nothing else here would do it.** The game animates its lights in the update
        /// traversal, and the harness runs none over the cell it stages — so every lamp in a `shot`,
        /// a `view` and a `bench` burned at its resting brightness while the same lamp in the game
        /// flickered. The walk holds the world's clock already, and a light's animation is a
        /// function of that clock alone, so the walk is where the two can be made to agree.
        TEST(RtxSceneExtractorTest, aWalkMirrorsALampAtTheInstantItWasToldRatherThanAtRest)
        {
            ESM::Light record;
            record.mData.mRadius = 100;
            record.mData.mColor = 0x00FFFFFF;
            record.mData.mFlags = ESM::Light::PulseSlow;

            osg::ref_ptr<SceneUtil::LightSource> lamp = SceneUtil::createLightSource(
                SceneUtil::LightCommon(record), SceneUtil::Mask_Lighting, /*isExterior=*/false);

            const auto litAt = [&lamp](double seconds) {
                Rtx::SceneDesc scene;
                SceneExtractor extractor(scene);
                extractor.setSimulationTime(seconds);
                extractor.extract(*lamp, osg::Matrixf::identity(), 0);

                const std::span<const Rtx::Light> lights = scene.getLights();
                EXPECT_EQ(lights.size(), 1u);

                return lights.empty() ? 0.0f : lights[0].mIntensity.x();
            };

            float deepest = 0.0f;

            // A pulse turns once in three seconds. Eight samples across it put one within an eighth
            // of a turn of the peak, so the deepest is at least `0.35 * cos(pi / 8)` from rest.
            for (int i = 0; i < 8; ++i)
            {
                const float lit = litAt(static_cast<double>(i) * 0.375);

                // A pulse swings 0.35 either way about what the lamp radiates at rest.
                EXPECT_GE(lit, sWhiteLampAtHundred * 0.65f);
                EXPECT_LE(lit, sWhiteLampAtHundred * 1.35f);

                deepest = std::max(deepest, std::abs(lit - sWhiteLampAtHundred));
            }

            EXPECT_GT(deepest, sWhiteLampAtHundred * 0.32f) << "the walk mirrored the lamp at rest";

            // And the instant is the whole of what decides it, so two walks over one clock agree.
            EXPECT_EQ(litAt(1.25), litAt(1.25));
        }
    }
}
