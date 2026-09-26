#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <osg/AlphaFunc>
#include <osg/BlendFunc>
#include <osg/GL>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Image>
#include <osg/Matrix>
#include <osg/MatrixTransform>
#include <osg/Node>
#include <osg/NodeVisitor>
#include <osg/StateAttribute>
#include <osg/StateSet>
#include <osg/Texture2D>
#include <osg/Texture>
#include <osg/Uniform>
#include <osg/Vec3f>
#include <osg/Vec4f>
#include <osg/ref_ptr>
#include <osgUtil/UpdateVisitor>

#include <components/rtx/extractionstats.hpp>
#include <components/rtx/instancerecord.hpp>
#include <components/rtx/lightbuilder.hpp>
#include <components/rtx/material.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/specularlayout.hpp>
#include <components/rtx/surface.hpp>
#include <components/rtx/textureencoding.hpp>
#include <components/rtx/texturetable.hpp>
#include <components/rtx/texturewrap.hpp>
#include <components/sceneutil/material.hpp>
#include <components/sceneutil/statesetupdater.hpp>
#include <components/sceneutil/texmat.hpp>
#include <components/sceneutil/texturetype.hpp>
#include <components/vfs/pathutil.hpp>

#include "../support/allocations.hpp"
#include "../support/graphlight.hpp"
#include "fixture.hpp"

namespace Rtx::Testing
{
    namespace
    {
        /// The same controller as an update callback, which is how `NifOsg` hangs everything the
        /// content did not mark auto-play.
        ///
        /// `StateSetUpdater::applyUpdate` alternates the node's own state set between two copies of
        /// itself, one per traversal parity, so a mirror keying a material on that address adds one
        /// and sweeps one every frame for a surface that has not moved — and every placement
        /// standing on it has to be repointed each time.
        /// Morrowind scrolls lava, waterfalls and banners by moving a texture matrix rather than
        /// geometry, and the description records the two numbers that matrix was built from. The
        /// sampler takes `uv * xy + zw`, so the scale-about-the-middle has to be resolved on the way
        /// through — a surface scaled by two with no offset samples `uv * 2 - 0.5`, which is the
        /// middle of the texture staying put while its edges move outward.
        TEST_F(RtxSceneExtractorTest, aScrollingSurfaceCarriesItsUvTransformResolvedForTheSampler)
        {
            osg::ref_ptr<osg::Geometry> quad = makeQuad();
            paint(*quad->getOrCreateStateSet(), "lava.dds");

            // The matrix `NifOsg::UVController` builds for a scale of two by four and an offset of a
            // quarter by minus a half.
            const osg::Vec3f origin(0.5f, 0.5f, 0.0f);
            osg::Matrixf transform = osg::Matrixf::translate(origin);
            transform.preMultScale(osg::Vec3f(2.0f, 4.0f, 1.0f));
            transform.preMultTranslate(-origin);
            transform.setTrans(transform.getTrans() + osg::Vec3f(0.25f, -0.5f, 0.0f));
            SceneUtil::setupTexMatForStateSet(*quad->getOrCreateStateSet(), 0, transform);

            osg::ref_ptr<osg::Group> root = new osg::Group;
            root->addChild(quad);

            walk(*root);

            ASSERT_EQ(mScene.materials().getRows().size(), 1u);

            // 0.5 * (1 - 2) + 0.25 = -0.25, and 0.5 * (1 - 4) - 0.5 = -2.
            EXPECT_EQ(mScene.materials().getRows()[0].mTextureTransform, osg::Vec4f(2.0f, 4.0f, -0.25f, -2.0f));
        }

        /// The identity, and not by accident: every surface that does not scroll shares one sampler
        /// path with the ones that do, so the transform has to be a no-op rather than a branch.
        TEST_F(RtxSceneExtractorTest, aSurfaceThatDoesNotScrollCarriesTheIdentityTransform)
        {
            osg::ref_ptr<osg::Geometry> quad = makeQuad();
            paint(*quad->getOrCreateStateSet(), "stone.dds");

            osg::ref_ptr<osg::Group> root = new osg::Group;
            root->addChild(quad);

            walk(*root);

            ASSERT_EQ(mScene.materials().getRows().size(), 1u);
            EXPECT_EQ(mScene.materials().getRows()[0].mTextureTransform, osg::Vec4f(1.0f, 1.0f, 0.0f, 0.0f));
        }

        TEST_F(RtxSceneExtractorTest, aMaterialKeepsItsSlotWhileTheNodesOwnStateSetAlternates)
        {
            osg::ref_ptr<ColourController> controller = new ColourController;

            osg::ref_ptr<osg::Group> node = new osg::Group;
            node->addChild(makeQuad());
            node->addUpdateCallback(controller);

            osgUtil::UpdateVisitor update;

            for (unsigned int pass = 1; pass <= 4; ++pass)
            {
                update.setTraversalNumber(pass);
                node->accept(update);

                mScene.clearPlacement();
                const ExtractionStats found = walk(*node, 0, pass);
                const Retirement went = mExtractor.retire();

                // The first pass is where everything arrives; what is asserted is that no later one
                // is, and the parity has turned over twice by the last.
                if (pass == 1)
                    continue;

                EXPECT_EQ(found.mMaterialsAdded, 0u) << "pass " << pass;
                EXPECT_EQ(found.mMaterialsReused, 1u) << "pass " << pass;
                EXPECT_EQ(went.mMaterials, 0u) << "pass " << pass << ": swept is added again next frame";
            }

            EXPECT_EQ(mScene.materials().getRows().size(), 1u);
        }

        TEST_F(RtxSceneExtractorTest, degenerateTrianglesAreDropped)
        {
            // One real triangle and two zero-area ones, which is how a triangle strip restarts.
            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
            geometry->setVertexArray(makePositions({
                osg::Vec3f(0.0f, 0.0f, 0.0f),
                osg::Vec3f(1.0f, 0.0f, 0.0f),
                osg::Vec3f(1.0f, 1.0f, 0.0f),
            }));
            geometry->addPrimitiveSet(makeTriangles({ 0, 1, 2, 0, 0, 1, 2, 2, 2 }));

            walk(*geometry);

            EXPECT_EQ(mScene.meshes().getTriangleCount(), 1u);
        }

        TEST_F(RtxSceneExtractorTest, geometryWithNoTrianglesIsSkippedRatherThanAdded)
        {
            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
            geometry->setVertexArray(makePositions({ osg::Vec3f(0.0f, 0.0f, 0.0f) }));

            const ExtractionStats stats = walk(*geometry);

            EXPECT_EQ(stats.mSkippedEmpty, 1u);
            EXPECT_EQ(stats.mInstances, 0u);
            EXPECT_TRUE(mScene.meshes().getRows().empty());
        }

        /// A drawable that describes nothing inherits what the state sets above it say.
        ///
        /// **Folded down the chain, which is how OpenGL resolves it.** A NIF property on a node
        /// applies to every shape below it until another replaces it, and a drawable carrying a
        /// state set for some unrelated reason — a uniform and nothing else, which is common — does
        /// not lose the surface it inherits by having one.
        TEST_F(RtxSceneExtractorTest, aDrawableWithNoDescriptionInheritsTheNearestOneAbove)
        {
            osg::ref_ptr<osg::Group> parent = new osg::Group;
            paint(*parent->getOrCreateStateSet(), "textures/tx_stone_01.dds");

            osg::ref_ptr<osg::Geometry> quad = makeQuad();
            quad->getOrCreateStateSet()->addUniform(new osg::Uniform("useFalloff", false));
            parent->addChild(quad);

            walk(*parent);

            ASSERT_EQ(mScene.materials().getRows().size(), 1u);
            ASSERT_EQ(mScene.textures().getRows().size(), 1u);
            EXPECT_EQ(mScene.textures().getRows()[0].mPath, VFS::Path::NormalizedView("textures/tx_stone_01.dds"));
            EXPECT_EQ(mScene.materials().getRows()[0].mDiffuse, 0u);

            // Nothing on the chain turned culling off, so the surface shows one face: the scene
            // root culls, and only a record that says otherwise makes a surface two-sided.
            EXPECT_FALSE(mScene.materials().getRows()[0].mTwoSided);
        }

        /// A blend is what marks a cutout in this data, and it has to survive into the material.
        ///
        /// Morrowind's foliage, grates and banners are drawn with `NiAlphaProperty` over a texture
        /// whose alpha is all but binary; hardly anything in the game sets an alpha test. Losing
        /// the blend here loses every mask with it.
        TEST_F(RtxSceneExtractorTest, aBlendedSurfaceIsTracedAsACutoutAndAPlainOneIsNot)
        {
            const auto extractOne = [](bool blend) {
                osg::ref_ptr<osg::Geometry> quad = makeQuad();
                osg::StateSet& state = *quad->getOrCreateStateSet();
                paint(state, "textures/tx_leaves.dds");
                if (blend)
                {
                    state.setAttributeAndModes(new osg::BlendFunc, osg::StateAttribute::ON);
                }

                Rtx::SceneDesc scene;
                SceneExtractor extractor(scene);
                extractor.extract(*quad, osg::Matrixf::identity(), 0);

                EXPECT_EQ(scene.materials().getRows().size(), 1u);
                return scene.materials().getRows().front();
            };

            const Rtx::Material blended = extractOne(true);
            EXPECT_EQ(blended.mAlphaMode, AlphaMode::Blend);
            EXPECT_TRUE(blended.isCutout());

            const Rtx::Material plain = extractOne(false);
            EXPECT_EQ(plain.mAlphaMode, AlphaMode::Opaque);
            EXPECT_FALSE(plain.isCutout());
        }

        /// A surface that adds — `SRC_ALPHA, ONE` — is no cutout, no pane and no medium: it is
        /// placed under `MASK_ADDITIVE` alone, built non-opaque so the one query that gathers it
        /// walks every crossing, and met by nothing that shades. A `SRC_ALPHA, DST_ALPHA` reads
        /// the same, because the frame's alpha is one; `ONE, ONE` adds whole.
        TEST_F(RtxSceneExtractorTest, anAdditiveSurfaceIsPlacedOnItsOwnMaskAndMetByNothingThatShades)
        {
            const auto extractOne = [](GLenum source, GLenum destination, float opacity) {
                osg::ref_ptr<osg::Geometry> quad = makeQuad();
                osg::StateSet& state = *quad->getOrCreateStateSet();
                paint(state, "textures/vfx_alt_glow02.dds");
                state.setAttributeAndModes(new osg::BlendFunc(source, destination), osg::StateAttribute::ON);

                osg::ref_ptr<SceneUtil::Material> colours = new SceneUtil::Material;
                colours->setDiffuse(osg::Vec4f(1.0f, 1.0f, 1.0f, opacity));
                state.setAttribute(colours);

                Rtx::SceneDesc scene;
                SceneExtractor extractor(scene);
                extractor.extract(*quad, osg::Matrixf::identity(), 0);

                EXPECT_EQ(scene.materials().getRows().size(), 1u);
                const Rtx::Material material = scene.materials().getRows().front();

                std::vector<InstanceRecord> records;
                makeInstanceRecords(scene, records);
                EXPECT_EQ(records.size(), 1u);

                return std::pair(material, records.front());
            };

            const auto [adds, addsRecord] = extractOne(GL_SRC_ALPHA, GL_ONE, 0.5f);
            EXPECT_EQ(adds.mBlend, BlendKind::Add);
            EXPECT_TRUE(adds.isAdditive());
            EXPECT_FALSE(adds.isCutout());
            EXPECT_FALSE(adds.isTranslucent()) << "an alpha of a half on an additive surface covers nothing";
            EXPECT_FALSE(adds.isMedium());
            EXPECT_EQ(addsRecord.mMask, Shaders::MASK_ADDITIVE);
            EXPECT_TRUE(addsRecord.mAdditive);
            EXPECT_FALSE(addsRecord.mCutout);
            EXPECT_FALSE(addsRecord.mTranslucent);

            const auto [frameAlpha, frameRecord] = extractOne(GL_SRC_ALPHA, GL_DST_ALPHA, 1.0f);
            EXPECT_EQ(frameAlpha.mBlend, BlendKind::Add);
            EXPECT_EQ(frameRecord.mMask, Shaders::MASK_ADDITIVE);

            const auto [whole, wholeRecord] = extractOne(GL_ONE, GL_ONE, 1.0f);
            EXPECT_EQ(whole.mBlend, BlendKind::AddWhole);
            EXPECT_EQ(wholeRecord.mMask, Shaders::MASK_ADDITIVE);

            // And the pane it is not: the same alpha over `ONE_MINUS_SRC_ALPHA` is glass.
            const auto [over, overRecord] = extractOne(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, 0.5f);
            EXPECT_EQ(over.mBlend, BlendKind::Over);
            EXPECT_FALSE(over.isAdditive());
            EXPECT_TRUE(over.isTranslucent());
            EXPECT_EQ(overRecord.mMask, Shaders::MASK_STATIC);
            EXPECT_TRUE(overRecord.mTranslucent);
        }

        /// The white ambient the game gives a magic effect joins the material's glow, times the
        /// material's own ambient colour, which is where `objects.frag` puts `ambientColor *
        /// ambientLight`. An ambient of (0.5, 1, 0.25) under a white override adds its decode to the
        /// glow; with no override nothing is added.
        ///
        /// The numbers: `decodeColour` undoes the sRGB curve, so 0.5 is 0.2140411, 1 is 1 and
        /// 0.25 is 0.0508761; an emission of 0.25 with a multiplier of two is 0.1017522.
        TEST_F(RtxSceneExtractorTest, theAmbientTheGameOverridesJoinsTheGlowByTheMaterialsAmbient)
        {
            const auto extractOne = [](bool overridden) {
                osg::ref_ptr<osg::Geometry> quad = makeQuad();
                osg::StateSet& state = *quad->getOrCreateStateSet();

                osg::ref_ptr<SceneUtil::Material> colours = new SceneUtil::Material;
                colours->setAmbient(osg::Vec4f(0.5f, 1.0f, 0.25f, 1.0f));
                colours->setEmission(osg::Vec4f(0.25f, 0.25f, 0.25f, 1.0f));
                colours->setEmissiveMultiplier(2.0f);
                state.setAttribute(colours);
                if (overridden)
                    state.addUniform(new osg::Uniform("sun.ambient", osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f)));

                Rtx::SceneDesc scene;
                SceneExtractor extractor(scene);
                extractor.extract(*quad, osg::Matrixf::identity(), 0);
                EXPECT_EQ(scene.materials().getRows().size(), 1u);
                return scene.materials().getRows().front().mEmissiveColour;
            };

            const osg::Vec3f lit = extractOne(true);
            EXPECT_NEAR(lit.x(), 0.1017522f + 0.2140411f, 1.0e-6f);
            EXPECT_NEAR(lit.y(), 0.1017522f + 1.0f, 1.0e-6f);
            EXPECT_NEAR(lit.z(), 0.1017522f + 0.0508761f, 1.0e-6f);

            const osg::Vec3f unlit = extractOne(false);
            EXPECT_NEAR(unlit.x(), 0.1017522f, 1.0e-6f);
            EXPECT_NEAR(unlit.y(), 0.1017522f, 1.0e-6f);
            EXPECT_NEAR(unlit.z(), 0.1017522f, 1.0e-6f);
        }

        /// The environment map, its tint and the dark map with its unit all reach the material,
        /// each in its own slot, and a file bound clamped and bound repeating is two slots.
        TEST_F(RtxSceneExtractorTest, theEnvironmentAndDarkMapsReachTheMaterialAndAWrapIsASlotOfItsOwn)
        {
            osg::ref_ptr<osg::Geometry> quad = makeQuad();
            osg::StateSet& state = *quad->getOrCreateStateSet();
            paint(state, "textures/a_glass.dds");
            paint(state, "textures/tx_6th_dark.dds", TextureRole::Dark);
            paint(state, "textures/vfx_alt_envir.dds", TextureRole::Environment);
            state.addUniform(new osg::Uniform("envMapColor", osg::Vec4f(1.0f, 0.5f, 0.0f, 1.0f)));

            // The dark map at unit one, and the same file as the diffuse bound again clamped at
            // unit three — the loader binds one unit per texture and a wrap per texture.
            osg::ref_ptr<osg::Image> sameFile = new osg::Image;
            sameFile->setFileName("textures/a_glass.dds");
            osg::ref_ptr<osg::Texture2D> clamped = new osg::Texture2D(sameFile);
            clamped->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            clamped->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
            state.setTextureAttributeAndModes(3, clamped, osg::StateAttribute::ON);
            state.setTextureAttribute(3, new SceneUtil::TextureType("emissiveMap"), osg::StateAttribute::ON);

            SurfaceDescription described;
            describeStateSet(state, described);
            EXPECT_EQ(described.getTextureUse(SurfaceMap::Emissive).mWrap, TextureWrap::Clamp);
            EXPECT_EQ(described.getTexture(SurfaceMap::Emissive), sameFile.get());

            walk(*quad);

            ASSERT_EQ(mScene.materials().getRows().size(), 1u);
            const Rtx::Material& material = mScene.materials().getRows().front();
            EXPECT_NE(material.mDark, sNoIndex);
            EXPECT_EQ(material.mDarkUnit, 1);
            EXPECT_NE(material.mEnvironment, sNoIndex);
            EXPECT_NEAR(material.mEnvironmentColour.x(), 1.0f, 1.0e-6f);
            EXPECT_NEAR(material.mEnvironmentColour.y(), 0.2140411f, 1.0e-6f);
            EXPECT_NEAR(material.mEnvironmentColour.z(), 0.0f, 1.0e-6f);

            EXPECT_NE(material.mDiffuse, material.mEmissive) << "one file under two wraps is two slots";
            EXPECT_EQ(mScene.textures().getRows()[material.mDiffuse].mPath,
                mScene.textures().getRows()[material.mEmissive].mPath);
            EXPECT_EQ(mScene.textures().getRows()[material.mDiffuse].mWrap, TextureWrap::Repeat);
            EXPECT_EQ(mScene.textures().getRows()[material.mEmissive].mWrap, TextureWrap::Clamp);
            EXPECT_EQ(mScene.textures().getRows().size(), 4u);
        }

        /// **The companion maps reach the material as data, and the specular map only in the layout
        /// that names what its channels mean.** A normal map with height and one without are one map.
        /// A walk told nothing reads no specular map: the classic layout is the one OpenMW documents,
        /// and read as metalness and roughness it is wrong. **A normal map bound with its height is
        /// parallax**, and not on a cutout, whose hole the traversal finds with no eye to shift by.
        TEST_F(RtxSceneExtractorTest, theCompanionMapsReachTheMaterialAsDataAndTheSpecularMapOnlyInItsLayout)
        {
            const auto extractOne = [](SpecularLayout layout, TextureRole normalRole, bool cutout = false) {
                osg::ref_ptr<osg::Geometry> quad = makeQuad();
                osg::StateSet& state = *quad->getOrCreateStateSet();
                paint(state, "textures/tx_a_steel.dds");
                paint(state, "textures/tx_a_steel_nh.dds", normalRole);
                paint(state, "textures/tx_a_steel_spec.dds", TextureRole::Specular);
                if (cutout)
                    state.setAttributeAndModes(new osg::AlphaFunc(osg::AlphaFunc::GEQUAL, 0.5f));

                Rtx::SceneDesc scene;
                SceneExtractor extractor(scene);
                extractor.setSpecularLayout(layout);
                extractor.extract(*quad, osg::Matrixf::identity(), 0);

                EXPECT_EQ(scene.materials().getRows().size(), 1u);
                return std::pair{ scene.materials().getRows().front(),
                    std::vector<TextureRow>(scene.textures().getRows().begin(), scene.textures().getRows().end()) };
            };

            const auto [ignored, ignoredRows] = extractOne(SpecularLayout::Ignore, TextureRole::NormalHeight);
            ASSERT_NE(ignored.mNormal, sNoIndex);
            EXPECT_EQ(ignoredRows[ignored.mNormal].mPath, VFS::Path::NormalizedView("textures/tx_a_steel_nh.dds"));
            EXPECT_EQ(ignoredRows[ignored.mNormal].mEncoding, TextureEncoding::Data);
            EXPECT_EQ(ignoredRows[ignored.mDiffuse].mEncoding, TextureEncoding::Colour);
            EXPECT_EQ(ignored.mSpecular, sNoIndex);
            EXPECT_EQ(ignoredRows.size(), 2u) << "a specular map read by nothing takes no slot";
            EXPECT_TRUE(ignored.mParallax) << "a normal map with its height";

            const auto [read, readRows] = extractOne(SpecularLayout::MetalRoughness, TextureRole::Normal);
            ASSERT_NE(read.mNormal, sNoIndex);
            ASSERT_NE(read.mSpecular, sNoIndex);
            EXPECT_EQ(readRows[read.mSpecular].mPath, VFS::Path::NormalizedView("textures/tx_a_steel_spec.dds"));
            EXPECT_EQ(readRows[read.mSpecular].mEncoding, TextureEncoding::Data);
            EXPECT_EQ(readRows.size(), 3u);
            EXPECT_FALSE(read.mParallax) << "a normal map without one";

            const auto [cut, cutRows] = extractOne(SpecularLayout::Ignore, TextureRole::NormalHeight, true);
            EXPECT_TRUE(cut.isCutout());
            EXPECT_FALSE(cut.mParallax) << "a cutout, shifted where it shades and not where it is cut";
        }

        /// A controller that shows a different sheet every frame, out of `mSheets`, on the unit
        /// the enchantment's glow uses: `SceneUtil::GlowUpdater`, with its clock made explicit.
        class FlipController : public SceneUtil::StateSetUpdater
        {
        public:
            std::vector<osg::ref_ptr<osg::Image>> mSheets;
            std::size_t mShown = 0;

            /// Unit one by number and not by `paint`, which appends: the walk hands the controller
            /// a copy of the node's own state set, which already carries the diffuse at nought.
            void setDefaults(osg::StateSet* stateset) override
            {
                stateset->setTextureAttributeAndModes(1, new osg::Texture2D(mSheets.front()), osg::StateAttribute::ON);
                stateset->setTextureAttribute(1,
                    new SceneUtil::TextureType(std::string(sTextureRoleNames.name(TextureRole::Environment))),
                    osg::StateAttribute::ON);
            }

            void apply(osg::StateSet* stateset, osg::NodeVisitor*) override
            {
                stateset->setTextureAttribute(
                    1, new osg::Texture2D(mSheets[mShown]), osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            }
        };

        osg::ref_ptr<FlipController> makeFlip()
        {
            osg::ref_ptr<FlipController> controller = new FlipController;
            for (std::size_t sheet = 0; sheet < 32; ++sheet)
            {
                osg::ref_ptr<osg::Image> image = new osg::Image;
                image->setFileName("textures/magicitem/caust" + std::to_string(sheet) + ".dds");
                controller->mSheets.push_back(image);
            }
            return controller;
        }

        /// A shape wearing `state`: on the node the way `NifOsg` builds one, or on the drawable
        /// under it, which is the other place a walk can meet a state set.
        osg::ref_ptr<osg::Group> makeShape(osg::StateSet* state, const bool onDrawable = false)
        {
            osg::ref_ptr<osg::Geometry> quad = makeQuad();
            osg::ref_ptr<osg::Group> shape = new osg::Group;
            if (onDrawable)
                quad->setStateSet(state);
            else
                shape->setStateSet(state);
            shape->addChild(quad);
            return shape;
        }

        osg::ref_ptr<osg::StateSet> shapeState()
        {
            osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
            paint(*state, "textures/w_sword.dds");
            state->setAttribute(new SceneUtil::Material, osg::StateAttribute::ON);
            return state;
        }

        /// An animated material keeps every texture it has worn, so a controller that cycles
        /// thirty-two caustic sheets at sixteen a second — `SceneUtil::GlowUpdater` — takes no slot
        /// and gives none back on any frame after the first it showed each on. The material's death
        /// lets them all go.
        ///
        /// **The sheet cycles beside a diffuse that stays**, which is the shape the game's glow has:
        /// a ring sized for the cycle alone holds the diffuse in it too, and from the second cycle
        /// on every sheet change drops the sheet due next and takes it up again.
        TEST_F(RtxSceneExtractorTest, anAnimatedMaterialKeepsEveryTextureItHasWorn)
        {
            osg::ref_ptr<FlipController> controller = makeFlip();
            osg::ref_ptr<osg::Group> node = makeShape(shapeState());
            node->addUpdateCallback(controller);

            osgUtil::UpdateVisitor update;
            for (unsigned int frame = 1; frame <= 72; ++frame)
            {
                controller->mShown = (frame - 1) % 32;
                update.setTraversalNumber(frame);
                node->accept(update);

                mScene.clearPlacement();
                walk(*node, 0, frame);

                ASSERT_EQ(mScene.materials().getRows().size(), 1u) << "on frame " << frame;
                EXPECT_EQ(mScene.materials().getRows()[0].mDiffuse, 0u) << "on frame " << frame;
                EXPECT_EQ(mScene.materials().getRows()[0].mEnvironment, controller->mShown + 1)
                    << "the sheet shown on frame " << frame;

                // The diffuse and a slot per distinct sheet as each is first shown, and none given
                // back: on the second time round every slot is already there.
                const std::size_t worn = std::min<std::size_t>(frame, 32) + 1;
                EXPECT_EQ(mScene.textures().getRows().size(), worn) << "on frame " << frame;
                EXPECT_EQ(mScene.textures().getLiveCount(), worn) << "on frame " << frame;
                if (frame > 32)
                {
                    EXPECT_TRUE(mScene.textures().getArrived().empty()) << "a sheet arrived again on frame " << frame;
                    EXPECT_TRUE(mScene.textures().getFreed().empty()) << "a sheet was given back on frame " << frame;
                }

                mExtractor.retire();
                mScene.clearArrivals();
            }

            // The material goes, and every sheet with it.
            node->removeChild(0, 1);
            mScene.clearPlacement();
            walk(*node, 0, 73);
            mExtractor.retire();

            for (std::size_t slot = 0; slot < 33; ++slot)
                EXPECT_TRUE(mScene.textures().isFree(slot)) << "slot " << slot << " outlived the material";
        }

        /// What `MWRender::TransparencyUpdater` does: sets its blend and its two uniforms up, and
        /// writes the fade every frame with no test that the uniform is there.
        class FadeController : public SceneUtil::StateSetUpdater
        {
        public:
            float mFade = 1.0f;

            void setDefaults(osg::StateSet* stateset) override
            {
                stateset->setAttributeAndModes(new osg::BlendFunc, osg::StateAttribute::ON);
                stateset->addUniform(new osg::Uniform("alpha", 1.0f));
                stateset->addUniform(new osg::Uniform("actorFade", 1.0f));
            }

            void apply(osg::StateSet* stateset, osg::NodeVisitor*) override
            {
                stateset->getUniform("actorFade")->set(mFade);
            }
        };

        /// A fade that arrives after a glow finds its own defaults, both apply, and nothing the fade
        /// set stays once it goes.
        ///
        /// **What an actor casting and then going invisible does**: the glow rides the root's update
        /// chain and the fade its cull chain. A state set set up by the glow alone had no
        /// `actorFade`, and the fade dereferenced it on the next walk.
        TEST_F(RtxSceneExtractorTest, aFadeAfterAGlowFindsItsOwnDefaultsAndLeavesNothingWhenItGoes)
        {
            osg::ref_ptr<FlipController> glow = makeFlip();
            osg::ref_ptr<FadeController> fade = new FadeController;
            fade->mFade = 0.25f;

            osg::ref_ptr<osg::Group> root = makeShape(shapeState());
            root->addUpdateCallback(glow);

            osgUtil::UpdateVisitor update;
            const auto frame = [&](unsigned int number) {
                glow->mShown = number - 1;
                update.setTraversalNumber(number);
                root->accept(update);

                mScene.clearPlacement();
                walk(*root, 0, number);
                mExtractor.retire();
                mScene.clearArrivals();

                EXPECT_EQ(mScene.materials().getRows().front().mEnvironment, glow->mShown + 1)
                    << "the glow on frame " << number;
                return mScene.placements().getRows().front().mInstance.mOpacity;
            };

            EXPECT_EQ(frame(1), 1.0f);

            root->addCullCallback(fade);
            EXPECT_EQ(frame(2), 0.25f) << "the fade that arrived after the glow";
            EXPECT_EQ(frame(3), 0.25f);

            root->removeCullCallback(fade);
            EXPECT_EQ(frame(4), 1.0f) << "a fade that went, still applied";
        }

        /// A material read once keeps every map it names for as long as it stands. The walk's own
        /// hold on an image goes on the frame after the material arrived, so the row's is the one
        /// that lasts — and a map the row does not hold is freed under it, its slot handed to the
        /// next texture that arrives.
        TEST_F(RtxSceneExtractorTest, aMaterialReadOnceKeepsItsEnvironmentAndDarkMaps)
        {
            osg::ref_ptr<osg::Geometry> quad = makeQuad();
            osg::StateSet& state = *quad->getOrCreateStateSet();
            paint(state, "textures/a_glass.dds");
            paint(state, "textures/tx_6th_dark.dds", TextureRole::Dark);
            paint(state, "textures/vfx_alt_envir.dds", TextureRole::Environment);

            for (unsigned int frame = 1; frame <= 6; ++frame)
            {
                mScene.clearPlacement();
                walk(*quad, 0, frame);

                ASSERT_EQ(mScene.materials().getRows().size(), 1u) << "on frame " << frame;
                const Rtx::Material& material = mScene.materials().getRows()[0];
                EXPECT_EQ(material.mDiffuse, 0u);
                EXPECT_EQ(material.mEnvironment, 1u);
                EXPECT_EQ(material.mDark, 2u);
                EXPECT_EQ(mScene.textures().getLiveCount(), 3u) << "on frame " << frame;
                EXPECT_TRUE(mScene.textures().getFreed().empty()) << "a map was given back on frame " << frame;

                mExtractor.retire();
                mScene.clearArrivals();
            }
        }

        /// A glow the game puts on an instance's root is read into the shape under it, and the
        /// shape's state set is the one every instance of the model shares. So a shape under an
        /// animated state set is animated too and keyed on its own node: the enchanted sword's
        /// sheet cycles, and the plain sword of the same model beside it wears none.
        ///
        /// **Both places a walk meets a state set**, because the chain is what carries the answer
        /// and a drawable's own link is the last one on it.
        TEST_F(RtxSceneExtractorTest, aGlowAboveAShapeAnimatesItAndLeavesTheSameShapeElsewhereAlone)
        {
            for (const bool onDrawable : { false, true })
            {
                Rtx::SceneDesc scene;
                SceneExtractor extractor(scene);

                osg::ref_ptr<osg::StateSet> shared = shapeState();

                osg::ref_ptr<FlipController> controller = makeFlip();
                osg::ref_ptr<osg::Group> enchanted = new osg::Group;
                enchanted->addChild(makeShape(shared, onDrawable));
                enchanted->addUpdateCallback(controller);

                osg::ref_ptr<osg::Group> plain = new osg::Group;
                plain->addChild(makeShape(shared, onDrawable));

                osg::ref_ptr<osg::Group> both = new osg::Group;
                both->addChild(plain);
                both->addChild(enchanted);

                const char* const where = onDrawable ? " with the state set on the drawable" : "";

                osgUtil::UpdateVisitor update;
                for (unsigned int frame = 1; frame <= 40; ++frame)
                {
                    controller->mShown = (frame - 1) % 32;
                    update.setTraversalNumber(frame);
                    both->accept(update);

                    scene.clearPlacement();
                    extractor.extract(*both, osg::Matrixf::identity(), 0, frame);

                    ASSERT_EQ(scene.materials().getRows().size(), 2u) << "on frame " << frame << where;
                    ASSERT_EQ(scene.placements().getRows().size(), 2u) << "on frame " << frame << where;

                    const Rtx::Index plainMaterial = scene.placements().getRows()[0].mInstance.mMaterial;
                    const Rtx::Index glowingMaterial = scene.placements().getRows()[1].mInstance.mMaterial;
                    ASSERT_NE(plainMaterial, glowingMaterial) << "on frame " << frame << where;

                    const Rtx::Material& glowing = scene.materials().getRows()[glowingMaterial];
                    EXPECT_TRUE(glowing.mAnimated) << "on frame " << frame << where;
                    EXPECT_EQ(glowing.mEnvironment, controller->mShown + 1)
                        << "the sheet shown on frame " << frame << where;

                    const Rtx::Material& bare = scene.materials().getRows()[plainMaterial];
                    EXPECT_FALSE(bare.mAnimated) << "on frame " << frame << where;
                    EXPECT_EQ(bare.mEnvironment, Rtx::sNoIndex) << "on frame " << frame << where;
                    EXPECT_EQ(bare.mDiffuse, glowing.mDiffuse) << "on frame " << frame << where;

                    extractor.retire();
                    scene.clearArrivals();
                }
            }
        }

        /// An actor's fade rides its placement, and a model's own alpha does not ride it twice.
        ///
        /// **`alpha` has two writers and they mean different things.** `MWRender::TransparencyUpdater`
        /// writes it beside `actorFade` on a state set above the whole actor, which is where the
        /// distance fade, Invisibility and Chameleon all arrive. `NifOsg::AlphaController` writes it
        /// alone, and that one is the surface's own opacity — read into the material, and not into
        /// the placement as well, so a walk that took every `alpha` it met would not fade an
        /// animated surface twice. The pair is what tells the two apart.
        ///
        /// **On the placement and never on the material**, which is the half that cannot be got
        /// wrong: OpenMW's clone keeps state sets by reference, so every actor built from one body
        /// part reads one material, and a fade written there would fade all of them.
        TEST_F(RtxSceneExtractorTest, anActorsFadeRidesItsPlacementAndAModelsOwnAlphaDoesNot)
        {
            const auto extractOne = [](float alpha, std::optional<float> actorFade, float opacity) {
                osg::ref_ptr<osg::Group> parent = new osg::Group;
                osg::StateSet& above = *parent->getOrCreateStateSet();
                above.addUniform(new osg::Uniform("alpha", alpha));
                if (actorFade.has_value())
                    above.addUniform(new osg::Uniform("actorFade", *actorFade));

                osg::ref_ptr<osg::Geometry> quad = makeQuad();
                osg::StateSet& own = *quad->getOrCreateStateSet();
                paint(own, "textures/tx_a_imperial_helmet.dds");
                own.setAttributeAndModes(new osg::BlendFunc, osg::StateAttribute::ON);
                parent->addChild(quad);

                Rtx::SceneDesc scene;
                SceneExtractor extractor(scene);
                extractor.extract(*parent, osg::Matrixf::identity(), 0);

                EXPECT_EQ(scene.placements().getRows().size(), 1u);
                EXPECT_EQ(scene.materials().getRows().size(), 1u);

                // The material is asked as well, because the fade landing there instead would pass
                // every other assertion in this test.
                EXPECT_EQ(scene.materials().getRows().front().mOpacity, opacity)
                    << "a shared material took one actor's fade";

                std::vector<Rtx::InstanceRecord> records;
                Rtx::makeInstanceRecords(scene, records);
                EXPECT_EQ(records.size(), 1u);
                // A fade is not a hole: traversal still stops for the placement, as a cutout where
                // it is whole and as a translucent surface where it is not, and `candidateStops`
                // reads the mask's holes off the material either way. One of the two and not both,
                // because a translucent row is never counted as a cutout (`PlacedTraversal`).
                EXPECT_TRUE(records.front().mCutout || records.front().mTranslucent)
                    << "a fade let traversal commit the mask's holes";
                EXPECT_NE(records.front().mCutout, records.front().mTranslucent);

                return scene.placements().getRows().front().mInstance.mOpacity;
            };

            // Halves and quarters, so the product is exact in binary and the assertion is the
            // arithmetic rather than a tolerance: this is `objects.frag`'s own `alpha * actorFade`.
            EXPECT_EQ(extractOne(0.5f, 0.25f, 1.0f), 0.125f);
            EXPECT_EQ(extractOne(1.0f, 1.0f, 1.0f), 1.0f) << "an actor nothing is hiding";
            EXPECT_EQ(extractOne(0.5f, std::nullopt, 0.5f), 1.0f)
                << "a model animating its own alpha, counted once and in the material";
        }

        /// A placement keeps its slot while its fade changes, which is the only way a fade arrives.
        ///
        /// An actor fades over the last tenth of `actors processing range`, which is tens of frames
        /// of one placement standing in one slot — and a slot is what a hit reads back, so it cannot
        /// be replaced to carry a new number. A record built from the faded slot is translucent, and
        /// that is what forces the candidate loop traversal would otherwise skip.
        TEST_F(RtxSceneExtractorTest, aPlacementKeepsItsSlotWhileItsFadeChanges)
        {
            osg::ref_ptr<osg::Group> parent = new osg::Group;
            osg::StateSet& above = *parent->getOrCreateStateSet();
            osg::ref_ptr<osg::Uniform> fade = new osg::Uniform("actorFade", 1.0f);
            above.addUniform(fade);
            above.addUniform(new osg::Uniform("alpha", 1.0f));
            parent->addChild(makeQuad());

            walk(*parent);

            std::vector<Rtx::InstanceRecord> records;
            ASSERT_EQ(mScene.placements().getRows().size(), 1u);
            EXPECT_EQ(mScene.placements().getRows().front().mInstance.mOpacity, 1.0f);
            Rtx::makeInstanceRecords(mScene, records);
            EXPECT_FALSE(records.front().mTranslucent) << "an actor at full brightness stops every ray";

            mScene.placements().advance();
            fade->set(0.25f);
            walk(*parent);

            EXPECT_EQ(mScene.placements().getRows().size(), 1u) << "a second placement rather than the one that faded";
            EXPECT_EQ(mScene.placements().getRows().front().mInstance.mOpacity, 0.25f);

            // A fade is a row to rewrite — what traversal is told changed — and not a move: the
            // record carries no motion, or the actor would smear across the frame it faded on.
            ASSERT_EQ(mScene.placements().getMoved().size(), 1u)
                << "a placement that faded on the spot reported no row to write";
            EXPECT_EQ(mScene.placements().getMoved().front(), 0u);

            Rtx::makeInstanceRecords(mScene, records);
            EXPECT_TRUE(records.front().mTranslucent);
            EXPECT_EQ(records.front().mMotion, Rtx::toTransform3x4(osg::Matrixf::identity()))
                << "a fade on the spot carried a motion";
        }

        /// The emissive multiplier is folded into the colour, because their product is all the
        /// game's own shader ever uses — and it is folded in *past* the decode, because a
        /// multiplier is a gain on the light and not a colour of its own.
        ///
        /// The content states `(0.5, 0.25, 0)` in the space it was authored in, and the sRGB curve
        /// takes that to `(0.2140411, 0.0508761, 0)`. Within a millionth, because the curve is a
        /// `pow` and the numbers here are decimals.
        TEST_F(RtxSceneExtractorTest, anEmissiveMultiplierIsFoldedIntoTheColourItScales)
        {
            const auto extractOne = [](float multiplier) {
                osg::ref_ptr<osg::Geometry> quad = makeQuad();
                osg::StateSet& state = *quad->getOrCreateStateSet();

                SceneUtil::Material& surface = colours(state);
                surface.setEmission(osg::Vec4f(0.5f, 0.25f, 0.0f, 1.0f));
                surface.setEmissiveMultiplier(multiplier);

                Rtx::SceneDesc scene;
                SceneExtractor extractor(scene);
                extractor.extract(*quad, osg::Matrixf::identity(), 0);

                EXPECT_EQ(scene.materials().getRows().size(), 1u);
                return scene.materials().getRows().front().mEmissiveColour;
            };

            const auto expectScaled = [&](float multiplier) {
                const osg::Vec3f got = extractOne(multiplier);
                EXPECT_NEAR(got.x(), 0.2140411f * multiplier, 1e-6f) << "at " << multiplier;
                EXPECT_NEAR(got.y(), 0.0508761f * multiplier, 1e-6f) << "at " << multiplier;
                EXPECT_EQ(got.z(), 0.0f) << "at " << multiplier;
            };

            expectScaled(2.0f);
            expectScaled(0.5f);

            // The default is one, so a model that asked for nothing keeps the colour it authored.
            expectScaled(1.0f);
        }

        /// A glowing surface earns no lamp, and a `LightSource` beside it is what does the lighting.
        ///
        /// **Morrowind lights what it means to light with a `LIGH` record**, and a glow is a texture.
        /// `EMISSIVE_INTENSITY` says what a glow is worth and why it is worth no lamp.
        ///
        /// The record's own lamp is still there and still derives its two sizes from the radius the
        /// record states: a flame a sixteenth of it, and the fitting around that flame a quarter.
        TEST_F(RtxSceneExtractorTest, aGlowingSurfaceEarnsNoLampAndARecordDoesTheLighting)
        {
            const auto lampsOf = [](bool torch) {
                osg::ref_ptr<osg::MatrixTransform> root = new osg::MatrixTransform(
                    osg::Matrixf::scale(2.0f, 2.0f, 2.0f) * osg::Matrixf::translate(0.0f, 0.0f, 5.0f));

                osg::ref_ptr<osg::Geometry> quad = makeQuad();
                colours(*quad->getOrCreateStateSet()).setEmission(osg::Vec4f(0.5f, 0.25f, 0.0f, 1.0f));
                root->addChild(quad);
                if (torch)
                    root->addChild(makeLightSource(100.0f, osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f)));

                Rtx::SceneDesc scene;
                SceneExtractor extractor(scene);
                extractor.extract(*root, osg::Matrixf::identity(), 0);

                const std::span<const Light> lights = scene.lights();
                return std::vector<Light>(lights.begin(), lights.end());
            };

            EXPECT_TRUE(lampsOf(false).empty()) << "a glow on its own lights nothing";

            const std::vector<Light> lit = lampsOf(true);
            ASSERT_EQ(lit.size(), 1u) << "the record's own lamp and nothing beside it";
            EXPECT_EQ(lit.front().mSourceRadius, 100.0f / 16.0f) << "a record's flame is a sixteenth of its radius";
            EXPECT_EQ(lit.front().mClearance, 25.0f) << "and the fitting around it a quarter";
        }

        /// Two-sidedness is the `GL_CULL_FACE` mode the content turned off, and one face otherwise.
        ///
        /// **Off and never on.** OpenGL culls nothing unless told to and the scene root turns
        /// culling on globally, so a state set that says nothing shows one face — which is right
        /// for everything under that root — and only a `NiStencilProperty` drawing both faces, or a
        /// material file's flag, turns it off again.
        TEST_F(RtxSceneExtractorTest, aSurfaceIsTwoSidedWhenTheContentSaidSo)
        {
            const auto extractOne = [](bool twoSided) {
                osg::ref_ptr<osg::Geometry> quad = makeQuad();
                colours(*quad->getOrCreateStateSet());
                if (twoSided)
                    quad->getOrCreateStateSet()->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);

                Rtx::SceneDesc scene;
                SceneExtractor extractor(scene);
                extractor.extract(*quad, osg::Matrixf::identity(), 0);

                EXPECT_EQ(scene.materials().getRows().size(), 1u);
                return scene.materials().getRows()[0].mTwoSided;
            };

            EXPECT_TRUE(extractOne(true));
            EXPECT_FALSE(extractOne(false));
        }

        /// A card the content doubled for its back reaches the scene as one copy, marked a sheet.
        ///
        /// Eight vertices and four triangles, because that is how a leaf is spelled in the files:
        /// the back has vertices of its own, so the pair is found by position and not by index.
        /// `ShapeFold` says why the copy goes; this says the extractor asks it, keeps its answer on
        /// the mesh, and counts it.
        TEST_F(RtxSceneExtractorTest, aCardDoubledForItsBackIsFoldedToOneCopyAndMarkedASheet)
        {
            osg::ref_ptr<osg::Geometry> card = new osg::Geometry;
            card->setVertexArray(makePositions({
                osg::Vec3f(0.0f, 0.0f, 0.0f),
                osg::Vec3f(1.0f, 0.0f, 0.0f),
                osg::Vec3f(1.0f, 1.0f, 0.0f),
                osg::Vec3f(0.0f, 1.0f, 0.0f),
                osg::Vec3f(0.0f, 0.0f, 0.0f),
                osg::Vec3f(1.0f, 0.0f, 0.0f),
                osg::Vec3f(1.0f, 1.0f, 0.0f),
                osg::Vec3f(0.0f, 1.0f, 0.0f),
            }));
            card->addPrimitiveSet(makeTriangles({ 0, 1, 2, 0, 2, 3, 6, 5, 4, 7, 6, 4 }));
            colours(*card->getOrCreateStateSet());

            const ExtractionStats stats = walk(*card);

            EXPECT_EQ(stats.mSheets, 1u);
            ASSERT_EQ(mScene.meshes().getRows().size(), 1u);
            EXPECT_TRUE(mScene.meshes().getRows()[0].mShape.mSheet);
            EXPECT_EQ(mScene.meshes().getRows()[0].getTriangleCount(), 2u) << "the back is gone";
            EXPECT_EQ(mScene.meshes().getRows()[0].mVertices.mCount, 8u) << "its vertices stay; nothing points at them";

            // A plain quad is a quad: nothing paired, nothing dropped, not a sheet.
            osg::ref_ptr<osg::Geometry> quad = makeQuad();
            colours(*quad->getOrCreateStateSet());

            Rtx::SceneDesc plain;
            SceneExtractor other(plain);
            EXPECT_EQ(other.extract(*quad, osg::Matrixf::identity(), 0).mSheets, 0u);
            EXPECT_FALSE(plain.meshes().getRows()[0].mShape.mSheet);
            EXPECT_EQ(plain.meshes().getRows()[0].getTriangleCount(), 2u);
        }

        TEST_F(RtxSceneExtractorTest, aDrawableTheCallerCallsWaterIsShadedAsWaterAndTheRestAreNot)
        {
            constexpr osg::Node::NodeMask sWater = 1u << 6;
            constexpr osg::Node::NodeMask sOther = 1u << 3;

            const auto quadWith = [&](osg::Node::NodeMask mask) {
                osg::ref_ptr<osg::Geometry> quad = makeQuad();
                quad->setNodeMask(mask);

                // A state set of its own, because a material is keyed on one and two quads sharing
                // one would be one material between them.
                paint(*quad->getOrCreateStateSet(), "textures/water/water00.dds");

                return quad;
            };

            osg::ref_ptr<osg::Group> root = new osg::Group;
            root->addChild(quadWith(sWater));
            root->addChild(quadWith(sOther));

            // **A drawable that never set a mask, which is nearly every one in the game.** OSG
            // defaults a node mask to all ones, so a test that asks whether the water's bit is
            // *among* a drawable's bits says yes to all of them — and the whole world came out
            // shaded as sea, refracting like jelly. What names the water is that no other pass may
            // see it.
            root->addChild(quadWith(~osg::Node::NodeMask{ 0 }));

            // Nothing said, so nothing is water — which is the harness, and every caller that places
            // an analytic sea of its own instead.
            {
                Rtx::SceneDesc scene;
                SceneExtractor silent(scene);
                silent.extract(*root, osg::Matrixf::identity(), 0);

                ASSERT_EQ(scene.materials().getRows().size(), 3u);
                for (const Rtx::Material& material : scene.materials().getRows())
                    EXPECT_EQ(material.mKind, Rtx::MaterialKind::Surface);
            }

            mExtractor.setWaterMask(sWater);
            walk(*root);

            ASSERT_EQ(mScene.materials().getRows().size(), 3u);
            EXPECT_EQ(mScene.materials().getRows()[0].mKind, Rtx::MaterialKind::Water);
            EXPECT_EQ(mScene.materials().getRows()[1].mKind, Rtx::MaterialKind::Surface)
                << "a mask the caller did not name made a surface into a sea";
            EXPECT_EQ(mScene.materials().getRows()[2].mKind, Rtx::MaterialKind::Surface)
                << "a drawable with the default mask was called water, which is every drawable";

            // **What being water is actually for.** A shadow ray has to pass through the surface, or
            // every shallow in the game is lit as though the sea were a wall; the mask is where the
            // record says so, and the material kind is where it comes from.
            std::vector<Rtx::InstanceRecord> records;
            Rtx::makeInstanceRecords(mScene, records);

            ASSERT_EQ(records.size(), 3u);
            EXPECT_EQ(records[0].mMask, Rtx::Shaders::MASK_WATER);
            EXPECT_EQ(records[1].mMask, Rtx::Shaders::MASK_STATIC);
            EXPECT_EQ(records[2].mMask, Rtx::Shaders::MASK_STATIC);
            EXPECT_NE(records[0].mMask, records[1].mMask);
        }

        /// A controller of the shape `NifOsg` builds out of a `NiUVController`: it moves the texture
        /// matrix every time it is applied, and keeps everything else the state set already says.
        class ScrollController : public SceneUtil::StateSetUpdater
        {
        public:
            void setDefaults(osg::StateSet* stateset) override
            {
                SceneUtil::setupTexMatForStateSet(*stateset, 0, osg::Matrixf{});
            }

            void apply(osg::StateSet* stateset, osg::NodeVisitor*) override
            {
                mOffset += 0.01f;
                SceneUtil::setupTexMatForStateSet(*stateset, 0, osg::Matrixf::translate(mOffset, 0.0f, 0.0f));
            }

        private:
            float mOffset = 0.0f;
        };

        /// A hundred crates wear one material.
        ///
        /// One drawable under two transforms is the shape `SceneUtil::CopyOp` makes of a model
        /// placed twice: nodes copied, the drawable and its state set shared. Both placements
        /// resolve to one material.
        TEST_F(RtxSceneExtractorTest, aCopyOfADrawableWearsTheSameMaterial)
        {
            osg::ref_ptr<osg::Geometry> quad = makeQuad();
            osg::StateSet& state = *quad->getOrCreateStateSet();
            paint(state, "textures/tx_leaves.dds");
            state.setAttributeAndModes(new osg::BlendFunc, osg::StateAttribute::ON);

            osg::ref_ptr<osg::Group> root = new osg::Group;
            for (const float x : { 0.0f, 10.0f })
            {
                osg::ref_ptr<osg::MatrixTransform> placed
                    = new osg::MatrixTransform(osg::Matrix::translate(x, 0.0, 0.0));
                placed->addChild(quad);
                root->addChild(placed);
            }

            const ExtractionStats stats = walk(*root);

            ASSERT_EQ(mScene.meshes().getRows().size(), 1u);
            ASSERT_EQ(mScene.materials().getRows().size(), 1u);
            EXPECT_FALSE(mScene.materials().getRows()[0].mAnimated);
            EXPECT_TRUE(mScene.materials().getRows()[0].isCutout());
            EXPECT_EQ(stats.mInstances, 2u);
            ASSERT_EQ(mScene.placements().getRows().size(), 2u);
            EXPECT_EQ(mScene.placements().getRows()[0].mInstance.mMaterial, 0u);
            EXPECT_EQ(mScene.placements().getRows()[1].mInstance.mMaterial, 0u);
        }

        /// A cutout under a controller is an animated material, so every placement of it reaches
        /// the any-hit.
        TEST_F(RtxSceneExtractorTest, aCutoutUnderAControllerIsAnimated)
        {
            osg::ref_ptr<osg::Group> node = new osg::Group;
            node->addChild(makeQuad());
            node->addUpdateCallback(new ScrollController);

            osg::StateSet& state = *node->getOrCreateStateSet();
            paint(state, "textures/tx_banner.dds");
            state.setAttributeAndModes(new osg::BlendFunc, osg::StateAttribute::ON);

            osgUtil::UpdateVisitor update;
            update.setTraversalNumber(1);
            node->accept(update);

            walk(*node, 0, 1);

            ASSERT_EQ(mScene.materials().getRows().size(), 1u);
            EXPECT_TRUE(mScene.materials().getRows()[0].mAnimated);
            EXPECT_TRUE(mScene.materials().getRows()[0].isCutout());
            ASSERT_EQ(mScene.meshes().getRows().size(), 1u);

            // And it stays animated on the frame after, when the material is read again: the flag
            // is a fact about the state set and not about what the controller wrote this time.
            update.setTraversalNumber(2);
            node->accept(update);
            mScene.clearPlacement();
            walk(*node, 0, 2);
            EXPECT_TRUE(mScene.materials().getRows()[0].mAnimated);
        }

        /// A placement wears the material of its own chain, whatever its mesh wore elsewhere.
        ///
        /// **The case the loader cannot produce, built by hand**: one drawable with no state set
        /// of its own, under two parents describing two surfaces. One mesh, two materials, and each
        /// placement wears its parent's.
        TEST_F(RtxSceneExtractorTest, aPlacementWearsTheMaterialOfItsOwnChain)
        {
            osg::ref_ptr<osg::Geometry> quad = makeQuad();

            osg::ref_ptr<osg::Group> root = new osg::Group;
            for (const char* const file : { "textures/tx_first.dds", "textures/tx_second.dds" })
            {
                osg::ref_ptr<osg::Group> parent = new osg::Group;
                paint(*parent->getOrCreateStateSet(), file);
                parent->addChild(quad);
                root->addChild(parent);
            }

            const ExtractionStats stats = walk(*root);

            ASSERT_EQ(mScene.meshes().getRows().size(), 1u);
            ASSERT_EQ(mScene.materials().getRows().size(), 2u);
            EXPECT_EQ(stats.mInstances, 2u);
            ASSERT_EQ(mScene.placements().getRows().size(), 2u);
            EXPECT_EQ(mScene.placements().getRows()[0].mInstance.mMaterial, 0u);
            EXPECT_EQ(mScene.placements().getRows()[1].mInstance.mMaterial, 1u);
        }

        /// A material a controller rewrites resolves its texture out of the image, not its name.
        ///
        /// **Every frame it is met, because that is what an animated material costs.** The state set
        /// is the same object and keeps its slot, and what is inside it is read again — so the
        /// texture is asked for again as well. Asking by path built a `VFS::Path::Normalized` off
        /// the heap each time, and Morrowind scrolls 432 surfaces in one Vivec cell.
        ///
        /// Three walks, because the entry has to survive a sweep and not only a frame: the second
        /// walk is where a cache would answer and the third is where a sweep that took the entry
        /// with it would show.
        TEST_F(RtxSceneExtractorTest, anAnimatedMaterialFindsOneTextureSlotEveryFrame)
        {
            osg::ref_ptr<osg::Image> banner = new osg::Image;
            banner->setFileName("textures/tx_banner.dds");

            osg::ref_ptr<ColourController> controller = new ColourController;
            controller->mDiffuse = banner;

            osg::ref_ptr<osg::Geometry> quad = makeQuad();
            osg::ref_ptr<osg::Group> node = new osg::Group;
            node->addChild(quad);
            node->addUpdateCallback(controller);

            osgUtil::UpdateVisitor update;
            for (unsigned int frame = 1; frame <= 3; ++frame)
            {
                update.setTraversalNumber(frame);
                node->accept(update);

                mScene.clearPlacement();

                const std::size_t before = Testing::getAllocationCount();
                walk(*node, 0, frame);
                const std::size_t spent = Testing::getAllocationCount() - before;

                // **What the first frame legitimately spends, and every frame after it must not.**
                // The first walk builds the scene: the tables, the identity entries and the one
                // string this texture's path is ever read from. A steady frame reads the same graph
                // and must reach the heap not at all.
                if (frame > 1)
                {
                    EXPECT_EQ(spent, 0u) << spent << " allocations on frame " << frame;
                }

                ASSERT_EQ(mScene.materials().getRows().size(), 1u) << "on frame " << frame;
                EXPECT_TRUE(mScene.materials().getRows()[0].mAnimated);
                EXPECT_EQ(mScene.materials().getRows()[0].mDiffuse, 0u) << "the same slot, on frame " << frame;
                EXPECT_EQ(mScene.textures().getRows().size(), 1u) << "a second slot arrived on frame " << frame;

                mExtractor.retire();
            }

            // **And the slot goes when the surface does.** The walk holds a reference of its own so
            // that a slot it names cannot be handed out under it, and a hold nothing gives back is a
            // texture the scene keeps for the rest of the run.
            node->removeChild(quad);
            mScene.clearPlacement();
            walk(*node, 0, 4);
            mExtractor.retire();

            EXPECT_TRUE(mScene.textures().isFree(0)) << "the walk's own hold outlived the surface";
        }

        /// **A texture a full table refused is not asked again until the table frees a slot.** An
        /// animated material asks for its images every frame, and each refusal built the image's
        /// path off the heap only to be refused again.
        ///
        /// Three walks against the full table, one beside a clamped surface, then one after a slot is
        /// freed: the second and third are where a refusal asked again would spend and count, and
        /// the fifth is where a remembered refusal that outlived the room it lacked would leave the
        /// surface untextured.
        ///
        /// **The refusal is the wrap's and not the file's.** Slot 5 holds the banner clamped, which
        /// the full table answers all the same, so a clamped surface wearing the banner image the
        /// animated one was refused under a repeat still finds it.
        TEST_F(RtxSceneExtractorTest, aRefusedTextureIsAskedAgainOnlyOnceASlotIsFreed)
        {
            TextureTable& textures = mScene.textures();
            constexpr Index clampedSlot = 5;
            for (std::size_t at = 0; at < TextureTable::sCapacity; ++at)
            {
                const Index slot = at == clampedSlot
                    ? textures.add(VFS::Path::NormalizedView("textures/tx_banner.dds"), TextureWrap::Clamp)
                    : textures.add(VFS::Path::Normalized("textures/tx_" + std::to_string(at) + ".dds"));
                ASSERT_EQ(slot, at);
                textures.hold(slot);
            }

            osg::ref_ptr<osg::Image> banner = new osg::Image;
            banner->setFileName("textures/tx_banner.dds");

            osg::ref_ptr<ColourController> controller = new ColourController;
            controller->mDiffuse = banner;

            osg::ref_ptr<osg::Group> node = new osg::Group;
            node->addChild(makeQuad());
            node->addUpdateCallback(controller);

            osg::ref_ptr<osg::Texture2D> clamp = new osg::Texture2D(banner);
            clamp->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            clamp->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
            osg::ref_ptr<osg::Geometry> clamped = makeQuad();
            clamped->getOrCreateStateSet()->setTextureAttributeAndModes(0, clamp, osg::StateAttribute::ON);
            clamped->getOrCreateStateSet()->setTextureAttribute(0,
                new SceneUtil::TextureType(std::string(sTextureRoleNames.name(TextureRole::Diffuse))),
                osg::StateAttribute::ON);

            osgUtil::UpdateVisitor update;
            for (unsigned int frame = 1; frame <= 3; ++frame)
            {
                update.setTraversalNumber(frame);
                node->accept(update);
                mScene.clearPlacement();

                const std::size_t before = Testing::getAllocationCount();
                walk(*node, 0, frame);
                const std::size_t spent = Testing::getAllocationCount() - before;

                if (frame > 1)
                {
                    EXPECT_EQ(spent, 0u) << spent << " allocations on frame " << frame;
                }

                EXPECT_EQ(textures.getRefused(), 1u) << "asked again on frame " << frame;
                ASSERT_EQ(mScene.materials().getRows().size(), 1u);
                EXPECT_EQ(mScene.materials().getRows()[0].mDiffuse, sNoIndex);

                mExtractor.retire();
            }

            osg::ref_ptr<osg::Group> both = new osg::Group;
            both->addChild(node);
            both->addChild(clamped);

            update.setTraversalNumber(4);
            both->accept(update);
            mScene.clearPlacement();
            walk(*both, 0, 4);
            mExtractor.retire();

            EXPECT_EQ(textures.getRefused(), 1u);
            ASSERT_EQ(mScene.materials().getRows().size(), 2u);
            EXPECT_EQ(mScene.materials().getRows()[0].mDiffuse, sNoIndex);
            EXPECT_EQ(mScene.materials().getRows()[1].mDiffuse, clampedSlot) << "refused for another wrap's want";

            textures.drop(9);

            update.setTraversalNumber(5);
            both->accept(update);
            mScene.clearPlacement();
            walk(*both, 0, 5);

            EXPECT_EQ(textures.getRefused(), 1u);
            ASSERT_EQ(mScene.materials().getRows().size(), 2u);
            EXPECT_EQ(mScene.materials().getRows()[0].mDiffuse, 9u) << "the freed slot is the refused texture's";
        }

        /// The surface is read from its controller every frame, and from whichever controller the
        /// node carries now.
        ///
        /// **Which controller a node hangs off is a fact about this frame and not about the entry.**
        /// The walk holds a state set per animated node so that the address is the same one next
        /// frame, and it would be easy to hold the controller that wrote into it beside that — but
        /// content is free to add or remove one, and a surface would then go on being animated by a
        /// controller the graph no longer has.
        ///
        /// Three different reds, so each walk has one answer and no other.
        TEST_F(RtxSceneExtractorTest, aSurfaceIsReadEachFrameAndFollowsAControllerSwappedUnderTheWalk)
        {
            osg::ref_ptr<ColourController> first = new ColourController;
            first->mRed = 0.25f;

            osg::ref_ptr<osg::Group> node = new osg::Group;
            node->addChild(makeQuad());
            node->addUpdateCallback(first);

            walk(*node, 0, 1);

            ASSERT_EQ(mScene.materials().getRows().size(), 1u);
            expectRed(mScene.materials().getRows()[0].mDiffuseColour, 0.0508761f);

            // What the entry holds is the state set and never what was written into it, so a second
            // walk reads the surface again.
            first->mRed = 0.5f;
            mScene.clearPlacement();
            walk(*node, 0, 2);

            ASSERT_EQ(mScene.materials().getRows().size(), 1u) << "the same surface is the same slot";
            expectRed(mScene.materials().getRows()[0].mDiffuseColour, 0.2140411f);

            // And the chain changes under it: the controller swapped under the walk is not the one
            // that painted the surface.
            osg::ref_ptr<ColourController> second = new ColourController;
            second->mRed = 0.75f;
            node->removeUpdateCallback(first);
            node->addUpdateCallback(second);

            mScene.clearPlacement();
            walk(*node, 0, 3);

            ASSERT_EQ(mScene.materials().getRows().size(), 1u);
            expectRed(mScene.materials().getRows()[0].mDiffuseColour, 0.5225216f);
        }
    }
}
