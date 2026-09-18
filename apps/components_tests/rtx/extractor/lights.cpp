#include "fixture.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <string>

#include <osg/BlendFunc>
#include <osg/GL>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Image>
#include <osg/Matrix>
#include <osg/MatrixTransform>
#include <osg/StateAttribute>
#include <osg/StateSet>
#include <osg/Uniform>
#include <osg/Vec3f>
#include <osg/Vec4f>
#include <osg/ref_ptr>

#include <components/esm3/loadligh.hpp>
#include <components/rtx/lightbuilder.hpp>
#include <components/rtx/material.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/sceneutil/lightcommon.hpp>
#include <components/sceneutil/lightcontroller.hpp>
#include <components/sceneutil/lightmanager.hpp>

#include "../graphlight.hpp"

namespace Rtx::Testing
{
    namespace
    {
        /// What a white lamp of radius 100 radiates, once `makeLight` has derived it: the square of
        /// the recorded radius by a quarter of pi, 7853.98, and white decodes to one.
        ///
        /// **Formed as `makeLight` forms it and not written down**, because the pulse's peak is held
        /// against it exactly. Six digits of it sat two thousandths under the float the walk
        /// produces, and a lamp whose phase put a sample on the peak read over the bound by that —
        /// the phase is the light's id, which is the count of lights the process made before it, so
        /// which tests ran first decided whether this one passed.
        const float sWhiteLampAtHundred = 100.0f * 100.0f * (0.25f * Shaders::PI);

        /// A magic bolt's light is sized by the area its spell states, in feet, and a spell with no
        /// area stays the bolt the game made.
        ///
        /// Sixty-six units is what `ProjectileManager` gives every bolt. Fifty feet is 50 by
        /// 21.333 = 1066.7 units, which is the larger, so the light is a lamp of that radius: its
        /// reach `1066.7 * 2 + 128 = 2261.3` and its intensity `1066.7^2 * 0.25 * pi = 893,657`
        /// on a white colour. A spark of no area keeps the bolt's own sixty-six, reaching
        /// `66 * 2 + 128 = 260`; and an area smaller than the bolt, one foot, is not a shrinking.
        TEST_F(RtxSceneExtractorTest, aBoltsLightReachesTheAreaItsSpellStates)
        {
            osg::ref_ptr<SceneUtil::LightSource> fireball = makeLightSource(66.0f, osg::Vec4f(1, 1, 1, 1));
            fireball->setUserValue(std::string(Rtx::sSpellAreaValue), 50.0f);

            osg::ref_ptr<SceneUtil::LightSource> spark = makeLightSource(66.0f, osg::Vec4f(1, 1, 1, 1));

            osg::ref_ptr<SceneUtil::LightSource> touch = makeLightSource(66.0f, osg::Vec4f(1, 1, 1, 1));
            touch->setUserValue(std::string(Rtx::sSpellAreaValue), 1.0f);

            osg::ref_ptr<osg::Group> flying = new osg::Group;
            flying->addChild(fireball);
            flying->addChild(spark);
            flying->addChild(touch);
            walk(*flying);

            ASSERT_EQ(mScene.lights().size(), 3u);
            const std::span<const Rtx::Light> lights = mScene.lights();
            EXPECT_NEAR(lights[0].mReach, 2261.33f, 0.01f);
            EXPECT_NEAR(lights[0].mIntensity.x(), 893657.0f, 100.0f);
            EXPECT_NEAR(lights[1].mReach, 260.0f, 0.01f) << "no area, the bolt's own sixty-six";
            EXPECT_EQ(lights[2].mReach, lights[1].mReach) << "an area under the bolt's own radius is not a shrinking";
        }

        /// A magic effect's glowing sheets light the world as one fill lamp of their own size and
        /// colour, and nothing outside an effect does.
        ///
        /// The sheet: a unit quad under `SRC_ALPHA, ONE`, its map every texel (255, 128, 0) at full
        /// alpha — (1, 0.21586, 0) in light — a white tint at half opacity, and the white ambient
        /// the game gives an effect over a white material ambient, which is a glow of one. So it
        /// radiates `1 * 0.5 * 8 = 4` red and `0.21586 * 0.5 * 8 = 0.86342` green per unit of
        /// area. Stood at (1000, 0, 0) a hundred times its size, its box runs from (1000, 0, 0)
        /// to (1100, 100, 0): a ball at (1050, 50, 0) fifty wide, and a lamp of `2 * pi * 2500 =
        /// 15,708` times the radiance by the gain of four — 251,327 red and 54,254 green —
        /// reaching eight radii.
        ///
        /// Beside it, under the same root, a sheet that blends over — blood — adds nothing; and
        /// the same glowing quad stood outside any effect is a glow the walk does not read.
        TEST_F(RtxSceneExtractorTest, anEffectsGlowingSheetsLightTheWorldAsOneLamp)
        {
            constexpr osg::Node::NodeMask sEffect = 1u << 1;

            osg::ref_ptr<osg::Image> map = new osg::Image;
            map->setFileName("textures/vfx_fireball01.tga");
            map->allocateImage(2, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE);
            for (std::size_t texel = 0; texel < 4; ++texel)
            {
                map->data()[texel * 4] = 255;
                map->data()[texel * 4 + 1] = 128;
                map->data()[texel * 4 + 2] = 0;
                map->data()[texel * 4 + 3] = 255;
            }

            const auto makeSheet = [&](GLenum destination) {
                osg::ref_ptr<osg::Geometry> quad = makeQuad();
                osg::StateSet& state = *quad->getOrCreateStateSet();
                paint(state, *map);
                state.setAttributeAndModes(new osg::BlendFunc(GL_SRC_ALPHA, destination), osg::StateAttribute::ON);
                state.addUniform(new osg::Uniform("sun.ambient", osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f)));

                SceneUtil::Material& material = colours(state);
                material.setDiffuse(osg::Vec4f(1.0f, 1.0f, 1.0f, 0.5f));
                material.setAmbient(osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f));
                return quad;
            };

            osg::ref_ptr<osg::Group> effect = new osg::Group;
            effect->setNodeMask(sEffect);
            effect->addChild(makeSheet(GL_ONE));
            effect->addChild(makeSheet(GL_ONE_MINUS_SRC_ALPHA));

            osg::ref_ptr<osg::MatrixTransform> stood = new osg::MatrixTransform(
                osg::Matrix::scale(100.0, 100.0, 100.0) * osg::Matrix::translate(1000.0, 0.0, 0.0));
            stood->addChild(effect);
            stood->addChild(makeSheet(GL_ONE));

            mExtractor.setClassMask(Rtx::InstanceClass::Effect, sEffect);
            const ExtractionStats stats = walk(*stood);

            EXPECT_EQ(stats.mLights, 1u);
            ASSERT_EQ(mScene.lights().size(), 1u);
            const Rtx::Light& lamp = mScene.lights().front();
            EXPECT_NEAR(lamp.mIntensity.x(), 251327.0f, 4.0f);
            EXPECT_NEAR(lamp.mIntensity.y(), 54254.0f, 4.0f);
            EXPECT_FLOAT_EQ(lamp.mIntensity.z(), 0.0f);
            EXPECT_EQ(lamp.mPosition, osg::Vec3f(1050.0f, 50.0f, 0.0f));
            EXPECT_FLOAT_EQ(lamp.mSourceRadius, 50.0f);
            EXPECT_EQ(lamp.mClearance, lamp.mSourceRadius);
            EXPECT_FLOAT_EQ(lamp.mReach, 400.0f);
            EXPECT_EQ(lamp.mFill, 1u);

            // The map was averaged once and every sheet that adds reads that mean, the one
            // outside the effect included, which lights nothing with it.
            ASSERT_EQ(mScene.materials().getRows().size(), 3u);
            for (const Rtx::Material& worn : mScene.materials().getRows())
            {
                if (worn.isAdditive())
                {
                    EXPECT_NEAR(worn.mDiffuseMean.y(), 0.21586f, 1e-5f);
                }
            }

            // The next frame reads the same lamp, and a walk that meets no effect reads none.
            mScene.clearPlacement();
            walk(*stood);
            ASSERT_EQ(mScene.lights().size(), 1u);
            EXPECT_NEAR(mScene.lights().front().mIntensity.x(), 251327.0f, 4.0f);

            mScene.clearPlacement();
            walk(*makeSheet(GL_ONE));
            EXPECT_TRUE(mScene.lights().empty());
        }

        /// A magic effect's flames light the world as the effect's lamp too, read after the walk
        /// with the other emitters, and an effect the game hung a light on — a bolt in flight —
        /// is lit by that light alone.
        ///
        /// The flames: the plume of `fixture.hpp`, scaled by two and stood at x = 100, drawn
        /// with a map every texel (255, 128, 0) at full alpha — (1, 0.21586, 0) in light, which is
        /// the mean. One sprite of radius 6, colour (1, 0.5, 0.25) decoded to (1, 0.21404, 0.05088)
        /// at a quarter of alpha, and one of radius 2, white and whole. Each is `r^2 * alpha` of
        /// `mean * colour`: `9 * (1, 0.046203, 0)` and `4 * (1, 0.21586, 0)`, summed
        /// `(13, 1.27927, 0)`; the disc's pi and `FLAME_INTENSITY`'s `8 / pi` leave eight, and the
        /// gain four, so the lamp is `(416, 40.937, 0)`, at the emitter's own ball — (100, 0, 12)
        /// and 8 wide, as `particles.cpp` measures it — reaching eight radii.
        ///
        /// The same plume under a root that also carries a light of 66 is one light, the game's:
        /// a lamp of `66^2 * 0.25 * pi = 3421.2` on white, reaching `66 * 2 + 128 = 260`, and no
        /// glow beside it. And the plume outside any effect is a flame the walk lights nothing
        /// with.
        TEST_F(RtxSceneExtractorTest, anEffectsFlamesLightTheWorldAsItsLampUnlessTheGameLitIt)
        {
            constexpr osg::Node::NodeMask sEffect = 1u << 1;

            osg::ref_ptr<osg::Image> map = new osg::Image;
            map->setFileName("textures/vfx_fireglow.tga");
            map->allocateImage(2, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE);
            for (std::size_t texel = 0; texel < 4; ++texel)
            {
                map->data()[texel * 4] = 255;
                map->data()[texel * 4 + 1] = 128;
                map->data()[texel * 4 + 2] = 0;
                map->data()[texel * 4 + 3] = 255;
            }

            const Plume flames = makePlume(
                osg::Matrix::scale(2.0, 2.0, 2.0) * osg::Matrix::translate(100.0, 0.0, 0.0), /*additive=*/true, map);
            emit(*flames.mParticles, osg::Vec3f(0.0f, 0.0f, 5.0f), 3.0f, osg::Vec4f(1.0f, 0.5f, 0.25f, 0.5f));
            emit(*flames.mParticles, osg::Vec3f(0.0f, 0.0f, 9.0f), 1.0f, osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f));

            osg::ref_ptr<osg::Group> effect = new osg::Group;
            effect->setNodeMask(sEffect);
            effect->addChild(flames.mRoot);

            mExtractor.setClassMask(Rtx::InstanceClass::Effect, sEffect);
            ExtractionStats stats = walk(*effect);

            EXPECT_EQ(stats.mLights, 1u);
            EXPECT_EQ(stats.mEmitters, 1u);
            ASSERT_EQ(mScene.lights().size(), 1u);
            const Rtx::Light& lamp = mScene.lights().front();
            EXPECT_NEAR(lamp.mIntensity.x(), 416.0f, 1e-2f);
            EXPECT_NEAR(lamp.mIntensity.y(), 40.937f, 1e-2f);
            EXPECT_FLOAT_EQ(lamp.mIntensity.z(), 0.0f);
            EXPECT_EQ(lamp.mPosition, osg::Vec3f(100.0f, 0.0f, 12.0f));
            EXPECT_FLOAT_EQ(lamp.mSourceRadius, 8.0f);
            EXPECT_EQ(lamp.mClearance, lamp.mSourceRadius);
            EXPECT_FLOAT_EQ(lamp.mReach, 64.0f);
            EXPECT_EQ(lamp.mFill, 1u);

            osg::ref_ptr<osg::Group> bolt = new osg::Group;
            bolt->setNodeMask(sEffect);
            bolt->addChild(flames.mRoot);
            bolt->addChild(makeLightSource(66.0f, osg::Vec4f(1, 1, 1, 1)));

            mScene.clearPlacement();
            stats = walk(*bolt);
            EXPECT_EQ(stats.mLights, 1u);
            ASSERT_EQ(mScene.lights().size(), 1u);
            EXPECT_NEAR(mScene.lights().front().mIntensity.x(), 3421.2f, 0.1f) << "the bolt's own, and no glow";
            EXPECT_NEAR(mScene.lights().front().mReach, 260.0f, 0.01f);
            EXPECT_EQ(mScene.lights().front().mFill, 0u);

            mScene.clearPlacement();
            stats = walk(*flames.mRoot);
            EXPECT_EQ(stats.mEmitters, 1u);
            EXPECT_TRUE(mScene.lights().empty()) << "a flame outside an effect lights nothing";
        }

        /// **What the walk asks a `LightSource` is what it radiates, and nothing else.**
        ///
        /// Two things it used to ask instead. `getEmpty` means the model this light hangs on has no
        /// geometry — a rasterizer's reason to skip drawing one, not a statement that the light is
        /// off — and a `LIGH` whose mesh is empty still burns. And the diffuse alone is not a light's
        /// colour: `Animation::setLightEffect` puts a glow light's whole colour in the ambient, so a
        /// Light spell read that way lit nothing at all.
        TEST_F(RtxSceneExtractorTest, aLightIsMirroredForWhatItRadiatesRatherThanForWhatItHangsOn)
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

            const ExtractionStats stats = walk(*where);

            EXPECT_EQ(stats.mLights, 3u);
            ASSERT_EQ(mScene.lights().size(), 3u);

            const std::span<const Rtx::Light> lights = mScene.lights();
            EXPECT_EQ(lights[0].mPosition, osg::Vec3f(10.0f, 20.0f, 30.0f)) << "a light stood somewhere else";
            EXPECT_EQ(lights[1].mPosition, osg::Vec3f(10.0f, 20.0f, 30.0f)) << "a light stood somewhere else";

            EXPECT_NEAR(lights[0].mIntensity.x(), sWhiteLampAtHundred, 0.01f);
            EXPECT_EQ(lights[1].mIntensity, lights[0].mIntensity) << "an empty model dimmed the light hanging on it";

            // The same lamp scaled by what 1.5 of ambient decodes to — and a fill, because the
            // ambient is the whole of what it radiates: a flame three quarters of a body wide stood
            // on the ground where the glow hangs, the ray kept clear of the whole of it, and four
            // radii of reach. `makeFill` says why.
            EXPECT_NEAR(lights[2].mIntensity.x(), sWhiteLampAtHundred * 2.53716f, 0.05f);
            EXPECT_EQ(lights[2].mFill, 1u);
            EXPECT_EQ(lights[2].mPosition, osg::Vec3f(10.0f, 20.0f, 126.0f)) << "the ball stands on the glow's place";
            EXPECT_EQ(lights[2].mSourceRadius, 96.0f);
            EXPECT_EQ(lights[2].mClearance, 96.0f);
            EXPECT_EQ(lights[2].mReach, 400.0f);

            EXPECT_EQ(lights[0].mFill, 0u) << "a lamp with a diffuse is no fill";
            EXPECT_EQ(lights[0].mSourceRadius, 100.0f / 16.0f);
        }

        /// A lamp the record says animates is mirrored at the instant the walk was told, not at rest.
        ///
        /// **Because nothing else here would do it.** The game animates its lights in the update
        /// traversal, and the harness runs none over the cell it stages — so every lamp in a `shot`,
        /// a `view` and a `bench` burned at its resting brightness while the same lamp in the game
        /// flickered. The walk holds the world's clock already, and a light's animation is a
        /// function of that clock alone, so the walk is where the two can be made to agree.
        TEST_F(RtxSceneExtractorTest, aWalkMirrorsALampAtTheInstantItWasToldRatherThanAtRest)
        {
            ESM::Light record;
            record.mData.mRadius = 100;
            record.mData.mColor = 0x00FFFFFF;
            record.mData.mFlags = ESM::Light::PulseSlow;

            osg::ref_ptr<SceneUtil::LightSource> lamp
                = SceneUtil::createLightSource(SceneUtil::LightCommon(record), sLightMask, /*isExterior=*/false);

            const auto litAt = [&lamp](double seconds) {
                Rtx::SceneDesc scene;
                SceneExtractor extractor(scene);
                extractor.setSimulationTime(seconds);
                extractor.extract(*lamp, osg::Matrixf::identity(), 0);

                const std::span<const Rtx::Light> lights = scene.lights();
                EXPECT_EQ(lights.size(), 1u);

                return lights.empty() ? 0.0f : lights[0].mIntensity.x();
            };

            float deepest = 0.0f;

            // A pulse turns once in three seconds. Eight samples across it put one within an eighth
            // of a turn of the peak, so the deepest is at least `0.35 * cos(pi / 8)` from rest.
            for (int i = 0; i < 8; ++i)
            {
                const float lit = litAt(static_cast<double>(i) * 0.375);

                // A pulse swings 0.35 either way about what the lamp radiates at rest. The bounds
                // are formed the way `lightBrightness` forms them, so the trough and the peak sit
                // on them to the bit.
                EXPECT_GE(lit, sWhiteLampAtHundred * (1.0f - 0.35f));
                EXPECT_LE(lit, sWhiteLampAtHundred * (1.0f + 0.35f));

                deepest = std::max(deepest, std::abs(lit - sWhiteLampAtHundred));
            }

            EXPECT_GT(deepest, sWhiteLampAtHundred * 0.32f) << "the walk mirrored the lamp at rest";

            // And the instant is the whole of what decides it, so two walks over one clock agree.
            EXPECT_EQ(litAt(1.25), litAt(1.25));
        }
    }
}
