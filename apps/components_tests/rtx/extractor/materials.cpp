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
        TEST(RtxSceneExtractorTest, aScrollingSurfaceCarriesItsUvTransformResolvedForTheSampler)
        {
            osg::ref_ptr<osg::Geometry> quad = makeQuad();
            paint(*quad->getOrCreateStateSet(), "lava.dds");

            Surface::Material& described = describe(*quad->getOrCreateStateSet());
            described.mTextureScale = osg::Vec2f(2.0f, 4.0f);
            described.mTextureOffset = osg::Vec2f(0.25f, -0.5f);

            osg::ref_ptr<osg::Group> root = new osg::Group;
            root->addChild(quad);

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            extractor.extract(*root, osg::Matrixf::identity(), 0);

            ASSERT_EQ(scene.getMaterials().size(), 1u);

            // 0.5 * (1 - 2) + 0.25 = -0.25, and 0.5 * (1 - 4) - 0.5 = -2.
            EXPECT_EQ(scene.getMaterials()[0].mTextureTransform, osg::Vec4f(2.0f, 4.0f, -0.25f, -2.0f));
        }

        /// The identity, and not by accident: every surface that does not scroll shares one sampler
        /// path with the ones that do, so the transform has to be a no-op rather than a branch.
        TEST(RtxSceneExtractorTest, aSurfaceThatDoesNotScrollCarriesTheIdentityTransform)
        {
            osg::ref_ptr<osg::Geometry> quad = makeQuad();
            paint(*quad->getOrCreateStateSet(), "stone.dds");

            osg::ref_ptr<osg::Group> root = new osg::Group;
            root->addChild(quad);

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            extractor.extract(*root, osg::Matrixf::identity(), 0);

            ASSERT_EQ(scene.getMaterials().size(), 1u);
            EXPECT_EQ(scene.getMaterials()[0].mTextureTransform, osg::Vec4f(1.0f, 1.0f, 0.0f, 0.0f));
        }

        TEST(RtxSceneExtractorTest, aMaterialKeepsItsSlotWhileTheNodesOwnStateSetAlternates)
        {
            osg::ref_ptr<ColourController> controller = new ColourController;

            osg::ref_ptr<osg::Group> node = new osg::Group;
            node->addChild(makeQuad());
            node->addUpdateCallback(controller);

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);

            osgUtil::UpdateVisitor update;

            for (unsigned int pass = 1; pass <= 4; ++pass)
            {
                update.setTraversalNumber(pass);
                node->accept(update);

                scene.clearPlacement();
                const ExtractionStats found = extractor.extract(*node, osg::Matrixf::identity(), 0, pass);
                const Retirement went = extractor.retire();

                // The first pass is where everything arrives; what is asserted is that no later one
                // is, and the parity has turned over twice by the last.
                if (pass == 1)
                    continue;

                EXPECT_EQ(found.mMaterialsAdded, 0u) << "pass " << pass;
                EXPECT_EQ(found.mMaterialsReused, 1u) << "pass " << pass;
                EXPECT_EQ(went.mMaterials, 0u) << "pass " << pass << ": swept is added again next frame";
            }

            EXPECT_EQ(scene.getMaterials().size(), 1u);
        }

        TEST(RtxSceneExtractorTest, degenerateTrianglesAreDropped)
        {
            // One real triangle and two zero-area ones, which is how a triangle strip restarts.
            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
            geometry->setVertexArray(makePositions({
                osg::Vec3f(0.0f, 0.0f, 0.0f),
                osg::Vec3f(1.0f, 0.0f, 0.0f),
                osg::Vec3f(1.0f, 1.0f, 0.0f),
            }));
            geometry->addPrimitiveSet(makeTriangles({ 0, 1, 2, 0, 0, 1, 2, 2, 2 }));

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            extractor.extract(*geometry, osg::Matrixf::identity(), 0);

            EXPECT_EQ(scene.getTriangleCount(), 1u);
        }

        TEST(RtxSceneExtractorTest, geometryWithNoTrianglesIsSkippedRatherThanAdded)
        {
            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
            geometry->setVertexArray(makePositions({ osg::Vec3f(0.0f, 0.0f, 0.0f) }));

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            const ExtractionStats stats = extractor.extract(*geometry, osg::Matrixf::identity(), 0);

            EXPECT_EQ(stats.mSkippedEmpty, 1u);
            EXPECT_EQ(stats.mInstances, 0u);
            EXPECT_TRUE(scene.getMeshes().empty());
        }

        /// A drawable that describes nothing inherits the nearest description above it.
        ///
        /// **Nearest, and whole.** A NIF property on a node applies to every shape below it until
        /// another replaces it, so `NifOsg` resolves each shape against everything above and stamps
        /// one complete answer. Walking back up for the first description found reproduces that,
        /// and a drawable carrying a state set for some unrelated reason — a `CullFace` and nothing
        /// else, which is common — does not lose the surface it inherits by having one.
        TEST(RtxSceneExtractorTest, aDrawableWithNoDescriptionInheritsTheNearestOneAbove)
        {
            osg::ref_ptr<osg::Group> parent = new osg::Group;
            paint(*parent->getOrCreateStateSet(), "textures/tx_stone_01.dds");

            osg::ref_ptr<osg::Geometry> quad = makeQuad();
            quad->getOrCreateStateSet()->setAttributeAndModes(
                new osg::CullFace(osg::CullFace::BACK), osg::StateAttribute::OFF);
            parent->addChild(quad);

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            extractor.extract(*parent, osg::Matrixf::identity(), 0);

            ASSERT_EQ(scene.getMaterials().size(), 1u);
            ASSERT_EQ(scene.getTextures().size(), 1u);
            EXPECT_EQ(scene.getTextures()[0], VFS::Path::NormalizedView("textures/tx_stone_01.dds"));
            EXPECT_EQ(scene.getMaterials()[0].mDiffuse, 0u);

            // **The description's answer and not the drawable's pipeline state.** The quad turns
            // culling off in its own state set and the description above it says nothing about
            // faces, so it is single-sided: the scene root culls, and only a record that says
            // otherwise makes a surface two-sided. Reading the mode back off the state set, as the
            // mirror used to, would answer the other way.
            EXPECT_FALSE(scene.getMaterials()[0].mTwoSided);
        }

        /// A blend is what marks a cutout in this data, and it has to survive into the material.
        ///
        /// Morrowind's foliage, grates and banners are drawn with `NiAlphaProperty` over a texture
        /// whose alpha is all but binary; hardly anything in the game sets an alpha test. Losing
        /// the blend here loses every mask with it.
        TEST(RtxSceneExtractorTest, aBlendedSurfaceIsTracedAsACutoutAndAPlainOneIsNot)
        {
            const auto extractOne = [](bool blend) {
                osg::ref_ptr<osg::Geometry> quad = makeQuad();
                osg::StateSet& state = *quad->getOrCreateStateSet();
                paint(state, "textures/tx_leaves.dds");
                if (blend)
                {
                    state.setAttributeAndModes(new osg::BlendFunc, osg::StateAttribute::ON);
                    describe(state).mAlphaMode = Surface::AlphaMode::Blend;
                }

                Rtx::SceneDesc scene;
                SceneExtractor extractor(scene);
                extractor.extract(*quad, osg::Matrixf::identity(), 0);

                EXPECT_EQ(scene.getMaterials().size(), 1u);
                return scene.getMaterials().front();
            };

            const Rtx::Material blended = extractOne(true);
            EXPECT_EQ(blended.mAlphaMode, Rtx::AlphaMode::Blend);
            EXPECT_TRUE(blended.isCutout());

            const Rtx::Material plain = extractOne(false);
            EXPECT_EQ(plain.mAlphaMode, Rtx::AlphaMode::Opaque);
            EXPECT_FALSE(plain.isCutout());
        }

        /// An actor's fade rides its placement, and a model's own alpha does not ride it twice.
        ///
        /// **`alpha` has two writers and they mean different things.** `MWRender::TransparencyUpdater`
        /// writes it beside `actorFade` on a state set above the whole actor, which is where the
        /// distance fade, Invisibility and Chameleon all arrive. `NifOsg::AlphaController` writes it
        /// alone, and writes the same number into the surface description as well — so a walk that
        /// took every `alpha` it met would fade an animated surface twice. The pair is what tells
        /// the two apart.
        ///
        /// **On the placement and never on the material**, which is the half that cannot be got
        /// wrong: OpenMW's clone keeps state sets by reference, so every actor built from one body
        /// part reads one material, and a fade written there would fade all of them.
        TEST(RtxSceneExtractorTest, anActorsFadeRidesItsPlacementAndAModelsOwnAlphaDoesNot)
        {
            const auto extractOne = [](float alpha, std::optional<float> actorFade) {
                osg::ref_ptr<osg::Group> parent = new osg::Group;
                osg::StateSet& above = *parent->getOrCreateStateSet();
                above.addUniform(new osg::Uniform("alpha", alpha));
                if (actorFade.has_value())
                    above.addUniform(new osg::Uniform("actorFade", *actorFade));

                osg::ref_ptr<osg::Geometry> quad = makeQuad();
                osg::StateSet& own = *quad->getOrCreateStateSet();
                paint(own, "textures/tx_a_imperial_helmet.dds");
                own.setAttributeAndModes(new osg::BlendFunc, osg::StateAttribute::ON);
                describe(own).mAlphaMode = Surface::AlphaMode::Blend;
                parent->addChild(quad);

                Rtx::SceneDesc scene;
                SceneExtractor extractor(scene);
                extractor.extract(*parent, osg::Matrixf::identity(), 0);

                EXPECT_EQ(scene.getInstances().size(), 1u);
                EXPECT_EQ(scene.getMaterials().size(), 1u);

                // The material is asked as well, because the fade landing there instead would pass
                // every other assertion in this test.
                EXPECT_EQ(scene.getMaterials().front().mDiffuseColour.a(), 1.0f)
                    << "a shared material took one actor's fade";

                std::vector<Rtx::InstanceRecord> records;
                Rtx::makeInstanceRecords(scene, records);
                EXPECT_EQ(records.size(), 1u);
                EXPECT_TRUE(records.front().mCutout) << "a fade is not a hole, and the mask still has some";

                return scene.getInstances().front().mOpacity;
            };

            // Halves and quarters, so the product is exact in binary and the assertion is the
            // arithmetic rather than a tolerance: this is `objects.frag`'s own `alpha * actorFade`.
            EXPECT_EQ(extractOne(0.5f, 0.25f), 0.125f);
            EXPECT_EQ(extractOne(1.0f, 1.0f), 1.0f) << "an actor nothing is hiding";
            EXPECT_EQ(extractOne(0.5f, std::nullopt), 1.0f) << "a model animating its own alpha, counted once";
        }

        /// A placement keeps its slot while its fade changes, which is the only way a fade arrives.
        ///
        /// An actor fades over the last tenth of `actors processing range`, which is tens of frames
        /// of one placement standing in one slot — and a slot is what a hit reads back, so it cannot
        /// be replaced to carry a new number. A record built from the faded slot is translucent, and
        /// that is what forces the candidate loop traversal would otherwise skip.
        TEST(RtxSceneExtractorTest, aPlacementKeepsItsSlotWhileItsFadeChanges)
        {
            osg::ref_ptr<osg::Group> parent = new osg::Group;
            osg::StateSet& above = *parent->getOrCreateStateSet();
            osg::ref_ptr<osg::Uniform> fade = new osg::Uniform("actorFade", 1.0f);
            above.addUniform(fade);
            above.addUniform(new osg::Uniform("alpha", 1.0f));
            parent->addChild(makeQuad());

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            extractor.extract(*parent, osg::Matrixf::identity(), 0);

            std::vector<Rtx::InstanceRecord> records;
            ASSERT_EQ(scene.getInstances().size(), 1u);
            EXPECT_EQ(scene.getInstances().front().mOpacity, 1.0f);
            Rtx::makeInstanceRecords(scene, records);
            EXPECT_FALSE(records.front().mTranslucent) << "an actor at full brightness stops every ray";

            scene.advancePlacement();
            fade->set(0.25f);
            extractor.extract(*parent, osg::Matrixf::identity(), 0);

            EXPECT_EQ(scene.getInstances().size(), 1u) << "a second placement rather than the one that faded";
            EXPECT_EQ(scene.getInstances().front().mOpacity, 0.25f);

            // A fade is a row to rewrite — what traversal is told changed — and not a move: the
            // record carries no motion, or the actor would smear across the frame it faded on.
            ASSERT_EQ(scene.getMoved().size(), 1u) << "a placement that faded on the spot reported no row to write";
            EXPECT_EQ(scene.getMoved().front(), 0u);

            Rtx::makeInstanceRecords(scene, records);
            EXPECT_TRUE(records.front().mTranslucent);
            EXPECT_EQ(records.front().mMotion, Rtx::toTransform3x4(osg::Matrixf::identity()))
                << "a fade on the spot carried a motion";
        }

        /// The emissive multiplier is folded into the colour, because their product is all the
        /// game's own shader ever uses.
        TEST(RtxSceneExtractorTest, anEmissiveMultiplierIsFoldedIntoTheColourItScales)
        {
            const auto extractOne = [](float multiplier) {
                osg::ref_ptr<osg::Geometry> quad = makeQuad();
                osg::StateSet& state = *quad->getOrCreateStateSet();

                Surface::Material& surface = describe(state);
                surface.mEmissiveColour = osg::Vec3f(0.5f, 0.25f, 0.0f);
                surface.mEmissiveMult = multiplier;

                Rtx::SceneDesc scene;
                SceneExtractor extractor(scene);
                extractor.extract(*quad, osg::Matrixf::identity(), 0);

                EXPECT_EQ(scene.getMaterials().size(), 1u);
                return scene.getMaterials().front().mEmissiveColour;
            };

            EXPECT_EQ(extractOne(2.0f), osg::Vec3f(1.0f, 0.5f, 0.0f));
            EXPECT_EQ(extractOne(0.5f), osg::Vec3f(0.25f, 0.125f, 0.0f));

            // The default is one, so a model that asked for nothing keeps the colour it authored.
            EXPECT_EQ(extractOne(1.0f), osg::Vec3f(0.5f, 0.25f, 0.0f));
        }

        /// A glowing surface earns no lamp, and a `LightSource` beside it is what does the lighting.
        ///
        /// **Morrowind lights what it means to light with a `LIGH` record**, and a glow is a texture.
        /// `EMISSIVE_INTENSITY` says what a glow is worth and why it is worth no lamp.
        ///
        /// The record's own lamp is still there and still derives its two sizes from the radius the
        /// record states: a flame a sixteenth of it, and the fitting around that flame a quarter.
        TEST(RtxSceneExtractorTest, aGlowingSurfaceEarnsNoLampAndARecordDoesTheLighting)
        {
            const auto lampsOf = [](bool torch) {
                osg::ref_ptr<osg::MatrixTransform> root = new osg::MatrixTransform(
                    osg::Matrixf::scale(2.0f, 2.0f, 2.0f) * osg::Matrixf::translate(0.0f, 0.0f, 5.0f));

                osg::ref_ptr<osg::Geometry> quad = makeQuad();
                describe(*quad->getOrCreateStateSet()).mEmissiveColour = osg::Vec3f(0.5f, 0.25f, 0.0f);
                root->addChild(quad);
                if (torch)
                    root->addChild(makeLightSource(100.0f, osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f)));

                Rtx::SceneDesc scene;
                SceneExtractor extractor(scene);
                extractor.extract(*root, osg::Matrixf::identity(), 0);

                return std::vector<Light>(scene.getLights().begin(), scene.getLights().end());
            };

            EXPECT_TRUE(lampsOf(false).empty()) << "a glow on its own lights nothing";

            const std::vector<Light> lit = lampsOf(true);
            ASSERT_EQ(lit.size(), 1u) << "the record's own lamp and nothing beside it";
            EXPECT_EQ(lit.front().mSourceRadius, 100.0f / 16.0f) << "a record's flame is a sixteenth of its radius";
            EXPECT_EQ(lit.front().mClearance, 25.0f) << "and the fitting around it a quarter";
        }

        /// Two-sidedness is what the content said, not what the pipeline state happens to be.
        ///
        /// **This is the fact that used to be guessed.** OpenGL culls nothing unless told to and
        /// `NifOsg` only emitted a `CullFace` where a `NiStencilProperty` asked for one, so an
        /// absent attribute had to be read as two-sided — which is right for a sheet of vanilla
        /// foliage and wrong for everything under a scene root that turns culling on globally. The
        /// description says which, and says it whether or not any state set mentions culling.
        TEST(RtxSceneExtractorTest, aSurfaceIsTwoSidedWhenTheContentSaidSo)
        {
            const auto extractOne = [](bool twoSided) {
                osg::ref_ptr<osg::Geometry> quad = makeQuad();
                describe(*quad->getOrCreateStateSet()).mTwoSided = twoSided;

                Rtx::SceneDesc scene;
                SceneExtractor extractor(scene);
                extractor.extract(*quad, osg::Matrixf::identity(), 0);

                EXPECT_EQ(scene.getMaterials().size(), 1u);
                return scene.getMaterials()[0].mTwoSided;
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
        TEST(RtxSceneExtractorTest, aCardDoubledForItsBackIsFoldedToOneCopyAndMarkedASheet)
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
            describe(*card->getOrCreateStateSet());

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            const ExtractionStats stats = extractor.extract(*card, osg::Matrixf::identity(), 0);

            EXPECT_EQ(stats.mSheets, 1u);
            ASSERT_EQ(scene.getMeshes().size(), 1u);
            EXPECT_TRUE(scene.getMeshes()[0].mShape.mSheet);
            EXPECT_EQ(scene.getMeshes()[0].getTriangleCount(), 2u) << "the back is gone";
            EXPECT_EQ(scene.getMeshes()[0].mVertexCount, 8u) << "its vertices stay; nothing points at them";

            // A plain quad is a quad: nothing paired, nothing dropped, not a sheet.
            osg::ref_ptr<osg::Geometry> quad = makeQuad();
            describe(*quad->getOrCreateStateSet());

            Rtx::SceneDesc plain;
            SceneExtractor other(plain);
            EXPECT_EQ(other.extract(*quad, osg::Matrixf::identity(), 0).mSheets, 0u);
            EXPECT_FALSE(plain.getMeshes()[0].mShape.mSheet);
            EXPECT_EQ(plain.getMeshes()[0].getTriangleCount(), 2u);
        }

        TEST(RtxSceneExtractorTest, aDrawableTheCallerCallsWaterIsShadedAsWaterAndTheRestAreNot)
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

                ASSERT_EQ(scene.getMaterials().size(), 3u);
                for (const Rtx::Material& material : scene.getMaterials())
                    EXPECT_EQ(material.mKind, Rtx::MaterialKind::Surface);
            }

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            extractor.setWaterMask(sWater);
            extractor.extract(*root, osg::Matrixf::identity(), 0);

            ASSERT_EQ(scene.getMaterials().size(), 3u);
            EXPECT_EQ(scene.getMaterials()[0].mKind, Rtx::MaterialKind::Water);
            EXPECT_EQ(scene.getMaterials()[1].mKind, Rtx::MaterialKind::Surface)
                << "a mask the caller did not name made a surface into a sea";
            EXPECT_EQ(scene.getMaterials()[2].mKind, Rtx::MaterialKind::Surface)
                << "a drawable with the default mask was called water, which is every drawable";

            // **What being water is actually for.** A shadow ray has to pass through the surface, or
            // every shallow in the game is lit as though the sea were a wall; the mask is where the
            // record says so, and the material kind is where it comes from.
            std::vector<Rtx::InstanceRecord> records;
            Rtx::makeInstanceRecords(scene, records);

            ASSERT_EQ(records.size(), 3u);
            EXPECT_EQ(records[0].mMask, Rtx::Shaders::MASK_WATER);
            EXPECT_EQ(records[1].mMask, Rtx::Shaders::MASK_SOLID);
            EXPECT_EQ(records[2].mMask, Rtx::Shaders::MASK_SOLID);
            EXPECT_NE(records[0].mMask, records[1].mMask);
        }

        /// A controller of the shape `NifOsg` builds out of a `NiUVController`: it moves the
        /// description's texture offset every time it is applied, and keeps everything else the
        /// state set already says.
        class ScrollController : public SceneUtil::StateSetUpdater
        {
        public:
            void setDefaults(osg::StateSet*) override {}

            void apply(osg::StateSet* stateset, osg::NodeVisitor*) override
            {
                Surface::getWritableMaterial(*stateset)->mTextureOffset += osg::Vec2f(0.01f, 0.0f);
            }
        };

        /// A mesh records the material it arrived wearing, and a hundred crates wear it once.
        ///
        /// One drawable under two transforms is the shape `SceneUtil::CopyOp` makes of a model
        /// placed twice: nodes copied, the drawable and its state set shared. Both placements
        /// resolve to one material, and it is the mesh's.
        TEST(RtxSceneExtractorTest, aMeshRecordsTheMaterialItArrivedWearingAndACopyWearsTheSame)
        {
            osg::ref_ptr<osg::Geometry> quad = makeQuad();
            osg::StateSet& state = *quad->getOrCreateStateSet();
            paint(state, "textures/tx_leaves.dds");
            state.setAttributeAndModes(new osg::BlendFunc, osg::StateAttribute::ON);
            describe(state).mAlphaMode = Surface::AlphaMode::Blend;

            osg::ref_ptr<osg::Group> root = new osg::Group;
            for (const float x : { 0.0f, 10.0f })
            {
                osg::ref_ptr<osg::MatrixTransform> placed
                    = new osg::MatrixTransform(osg::Matrix::translate(x, 0.0, 0.0));
                placed->addChild(quad);
                root->addChild(placed);
            }

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            const ExtractionStats stats = extractor.extract(*root, osg::Matrixf::identity(), 0);

            ASSERT_EQ(scene.getMeshes().size(), 1u);
            ASSERT_EQ(scene.getMaterials().size(), 1u);
            EXPECT_EQ(scene.getMeshes()[0].mMaterial, 0u);
            EXPECT_FALSE(scene.getMaterials()[0].mAnimated);
            EXPECT_TRUE(scene.getMaterials()[0].isCutout());
            EXPECT_EQ(stats.mInstances, 2u);
            EXPECT_EQ(stats.mWornOtherwise, 0u);
            EXPECT_EQ(stats.mUnbakeable, 0u);
        }

        /// A cutout under a controller is an animated material, and every placement of it is one
        /// no bake can answer for.
        TEST(RtxSceneExtractorTest, aCutoutUnderAControllerIsAnimatedAndUnbakeable)
        {
            osg::ref_ptr<osg::Group> node = new osg::Group;
            node->addChild(makeQuad());
            node->addUpdateCallback(new ScrollController);

            osg::StateSet& state = *node->getOrCreateStateSet();
            paint(state, "textures/tx_banner.dds");
            state.setAttributeAndModes(new osg::BlendFunc, osg::StateAttribute::ON);
            describe(state).mAlphaMode = Surface::AlphaMode::Blend;

            osgUtil::UpdateVisitor update;
            update.setTraversalNumber(1);
            node->accept(update);

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            const ExtractionStats stats = extractor.extract(*node, osg::Matrixf::identity(), 0, 1);

            ASSERT_EQ(scene.getMaterials().size(), 1u);
            EXPECT_TRUE(scene.getMaterials()[0].mAnimated);
            EXPECT_TRUE(scene.getMaterials()[0].isCutout());
            ASSERT_EQ(scene.getMeshes().size(), 1u);
            EXPECT_EQ(scene.getMeshes()[0].mMaterial, 0u);
            EXPECT_EQ(stats.mUnbakeable, 1u);
            EXPECT_EQ(stats.mWornOtherwise, 0u);

            // And it stays animated on the frame after, when the material is read again: the flag
            // is a fact about the state set and not about what the controller wrote this time.
            update.setTraversalNumber(2);
            node->accept(update);
            scene.clearPlacement();
            extractor.extract(*node, osg::Matrixf::identity(), 0, 2);
            EXPECT_TRUE(scene.getMaterials()[0].mAnimated);
        }

        /// A placement wearing a material other than the one its mesh arrived with is counted.
        ///
        /// **The case the loader cannot produce, built by hand**: one drawable with no state set
        /// of its own, under two parents describing two surfaces. The mesh records the first, and
        /// the second placement is the canary.
        TEST(RtxSceneExtractorTest, aPlacementWearingAnotherMaterialThanItsMeshIsCounted)
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

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            const ExtractionStats stats = extractor.extract(*root, osg::Matrixf::identity(), 0);

            ASSERT_EQ(scene.getMeshes().size(), 1u);
            ASSERT_EQ(scene.getMaterials().size(), 2u);
            EXPECT_EQ(scene.getMeshes()[0].mMaterial, 0u) << "the material it arrived wearing";
            EXPECT_EQ(stats.mInstances, 2u);
            EXPECT_EQ(stats.mWornOtherwise, 1u);
        }
    }
}
