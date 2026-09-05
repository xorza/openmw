#include "fixture.hpp"

#include <algorithm>
#include <vector>

namespace Rtx::Testing
{
    namespace
    {
        /// A particle system under a transform that carries its texture and its blend, the way
        /// `NifOsg` builds one.
        ///
        /// The emitter's own state set sets neither, which is what makes this a test of the walk up
        /// the path rather than of the drawable: a `ParticleSystem` really does carry an empty state
        /// set of its own in the shipped content, and asking it for the blend answers "covers" for
        /// every flame in the game.
        struct Plume
        {
            osg::ref_ptr<osg::MatrixTransform> mRoot;
            osg::ref_ptr<osgParticle::ParticleSystem> mParticles;
        };

        Plume makePlume(const osg::Matrix& place, bool additive)
        {
            Plume plume;
            plume.mRoot = new osg::MatrixTransform(place);

            osg::StateSet& state = *plume.mRoot->getOrCreateStateSet();
            paint(state, "textures/tx_fire_00.dds");
            state.setAttributeAndModes(new osg::BlendFunc(osg::BlendFunc::SRC_ALPHA,
                                           additive ? osg::BlendFunc::ONE : osg::BlendFunc::ONE_MINUS_SRC_ALPHA),
                osg::StateAttribute::ON);
            describe(state).mAlphaMode = Surface::AlphaMode::Blend;

            plume.mParticles = new osgParticle::ParticleSystem;
            plume.mParticles->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
            plume.mRoot->addChild(plume.mParticles);

            return plume;
        }

        /// Adds one particle and brings its interpolated size, colour and alpha up to date.
        ///
        /// `getCurrentSize` and the two beside it are only meaningful after `Particle::update`, so
        /// the zero-length step is not a formality: without it every sprite this test reads back
        /// carries whatever the default template was constructed with.
        osgParticle::Particle* emit(
            osgParticle::ParticleSystem& particles, const osg::Vec3f& at, float size, const osg::Vec4f& colour)
        {
            osgParticle::Particle seed;
            osgParticle::Particle* particle = particles.createParticle(&seed);
            particle->setLifeTime(10.0f);
            particle->setPosition(at);
            particle->setVelocity(osg::Vec3f());
            particle->setSizeRange(osgParticle::rangef(size, size));
            particle->setColorRange(osgParticle::rangev4(colour, colour));
            particle->setAlphaRange(osgParticle::rangef(colour.a(), colour.a()));
            particle->update(0.0, false);
            return particle;
        }

        /// A particle system is not geometry, and what comes out of it is a run of discs.
        ///
        /// Every number here is the file's own carried through one transform: the placement moves
        /// each sprite and its uniform scale widens it, because `NifOsg` asks for particle sizes in
        /// the emitter's own coordinates and the modelview is what the rasterizer would have scaled
        /// them by.
        TEST_F(RtxSceneExtractorTest, aParticleSystemPlacesSpritesAndNoMesh)
        {
            // Scaled by two and moved a hundred along x, so the radius and the position each prove a
            // different half of the transform.
            const Plume plume = makePlume(osg::Matrix::scale(2.0, 2.0, 2.0) * osg::Matrix::translate(100.0, 0.0, 0.0),
                /*additive=*/true);

            emit(*plume.mParticles, osg::Vec3f(0.0f, 0.0f, 5.0f), 3.0f, osg::Vec4f(1.0f, 0.5f, 0.25f, 0.5f));
            emit(*plume.mParticles, osg::Vec3f(0.0f, 0.0f, 9.0f), 1.0f, osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f));

            const ExtractionStats stats = walk(*plume.mRoot);

            EXPECT_EQ(stats.mEmitters, 1u);
            EXPECT_EQ(stats.mSprites, 2u);
            EXPECT_EQ(stats.mSkippedUnknown, 0u) << "a particle system is read, not passed over";
            EXPECT_EQ(stats.mInstances, 0u) << "sprites are the drawing, so there is nothing to build over";
            EXPECT_EQ(stats.mMeshesAdded, 0u);

            ASSERT_EQ(mScene.getSprites().size(), 2u);

            // (0, 0, 5) scaled by two is (0, 0, 10), then moved to x = 100. The radius is the file's
            // three by the same two.
            const Rtx::Sprite& low = mScene.getSprites()[0];
            EXPECT_EQ(low.mPosition, osg::Vec3f(100.0f, 0.0f, 10.0f));
            EXPECT_FLOAT_EQ(low.mRadius, 6.0f);
            EXPECT_EQ(low.mColour, osg::Vec3f(1.0f, 0.5f, 0.25f));

            // The colour ramp's alpha and the alpha ramp are separate and the rasterizer multiplies
            // them; here both are a half, so a quarter is what proves the product rather than one of
            // the two being read and the other dropped.
            EXPECT_FLOAT_EQ(low.mAlpha, 0.25f);

            EXPECT_EQ(mScene.getSprites()[1].mPosition, osg::Vec3f(100.0f, 0.0f, 18.0f));
            EXPECT_FLOAT_EQ(mScene.getSprites()[1].mRadius, 2.0f);

            // Two sprites four apart before the scale and eight after, each one wider than the
            // other: the box runs z = 4 to 20, so the centre is 12 and the reach 8.
            ASSERT_EQ(mScene.getEmitters().size(), 1u);
            EXPECT_EQ(mScene.getEmitters().front().mCentre, osg::Vec3f(100.0f, 0.0f, 12.0f));
            EXPECT_FLOAT_EQ(mScene.getEmitters().front().mReach, 8.0f);

            // The texture, and beside it the bake of its alpha the sprites are lit by.
            ASSERT_EQ(mScene.getTextures().size(), 2u);
            EXPECT_EQ(mScene.getTextures()[0], VFS::Path::NormalizedView("textures/tx_fire_00.dds"));
            EXPECT_EQ(mScene.getBakedTextures()[1],
                SpriteLightMap::keyFor(VFS::Path::NormalizedView("textures/tx_fire_00.dds")));
            EXPECT_EQ(mScene.getEmitters().front().mTexture, 0u);
            EXPECT_EQ(mScene.getEmitters().front().mLighting, 1u);
        }

        /// A quad that hangs in the world hangs on the axis its own particle carries.
        ///
        /// **`osgParticle` turns both of a quad's axes by the angle the particle holds**, and
        /// `Weather::RainShooter` is what leans a raindrop into the wind that way — the shooter sets
        /// an angle off the wind speed as it fires each drop. An axis read from the system alone is
        /// the same for every drop, so a storm the rasterizer drew leaning fell straight down here,
        /// and it leant further as the wind rose in one renderer and not in the other.
        ///
        /// **And the placement turns that axis without scaling it**, because the sprite's radius
        /// already carries the scale: a quad reaches `mAxis * mRadius`, so an emitter that scaled
        /// both squared the scale.
        TEST_F(RtxSceneExtractorTest, aQuadHangsOnTheAxisItsOwnParticleCarries)
        {
            // Turned a quarter turn about z and doubled, so the axis proves the turn and the radius
            // proves the scale.
            const Plume rain = makePlume(
                osg::Matrix::scale(2.0, 2.0, 2.0) * osg::Matrix::rotate(osg::PI_2, osg::Vec3d(0.0, 0.0, 1.0)),
                /*additive=*/false);

            // Morrowind's own rain shape: an across axis squashed to a tenth against an axis
            // pointing straight down.
            rain.mParticles->setParticleAlignment(osgParticle::ParticleSystem::FIXED);
            rain.mParticles->setAlignVectors(osg::Vec3f(0.1f, 0.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, -1.0f));

            const osg::Vec4f white(1.0f, 1.0f, 1.0f, 1.0f);
            emit(*rain.mParticles, osg::Vec3f(), 3.0f, white);
            emit(*rain.mParticles, osg::Vec3f(), 3.0f, white)->setAngle(osg::Vec3f(osg::PIf / 6.0f, 0.0f, 0.0f));

            walk(*rain.mRoot);

            ASSERT_EQ(mScene.getEmitters().size(), 1u);
            ASSERT_EQ(mScene.getSprites().size(), 2u);

            // The width is the across axis's own length, and neither the turn nor the scale reaches
            // it — which is why it is the emitter's and is read once.
            EXPECT_FLOAT_EQ(mScene.getEmitters().front().mWidth, 0.1f);

            // A drop with no angle hangs where the content put it: a quarter turn about z leaves an
            // axis pointing down where it was, and the scale is the radius's alone.
            const Rtx::Sprite& straight = mScene.getSprites()[0];
            EXPECT_NEAR(straight.mAxis.x(), 0.0f, 1e-6f);
            EXPECT_NEAR(straight.mAxis.y(), 0.0f, 1e-6f);
            EXPECT_NEAR(straight.mAxis.z(), -1.0f, 1e-6f);
            EXPECT_FLOAT_EQ(straight.mRadius, 6.0f) << "the radius is the one thing the scale reaches";

            // Thirty degrees about x takes (0, 0, -1) to (0, sin 30, -cos 30), and the quarter turn
            // about z takes that to (-sin 30, 0, -cos 30).
            const Rtx::Sprite& leaning = mScene.getSprites()[1];
            EXPECT_NEAR(leaning.mAxis.x(), -0.5f, 1e-6f);
            EXPECT_NEAR(leaning.mAxis.y(), 0.0f, 1e-6f);
            EXPECT_NEAR(leaning.mAxis.z(), -0.8660254f, 1e-6f);

            // **A rotation cannot lengthen a streak**, so the drop that leans is the same drop.
            EXPECT_NEAR(leaning.mAxis.length(), 1.0f, 1e-6f);
            EXPECT_FLOAT_EQ(leaning.mRadius, straight.mRadius);
        }

        /// `SRC_ALPHA, ONE` is a flame and anything else covers, and the difference is what decides
        /// whether the sprite is light or an albedo to be lit.
        ///
        /// The blend sits on the transform above the emitter, where `NifOsg` puts it, and the
        /// emitter carries a state set of its own that says nothing about blending — so an answer
        /// read off the drawable is "covers" both times.
        TEST_F(RtxSceneExtractorTest, theBlendTellsAFlameFromSmoke)
        {
            const auto extractOne = [](bool additive) {
                const Plume plume = makePlume(osg::Matrix::identity(), additive);
                emit(*plume.mParticles, osg::Vec3f(), 1.0f, osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f));

                Rtx::SceneDesc scene;
                SceneExtractor extractor(scene);
                extractor.extract(*plume.mRoot, osg::Matrixf::identity(), 0);

                EXPECT_EQ(scene.getEmitters().size(), 1u);
                return scene.getEmitters().front().mAdditive;
            };

            EXPECT_TRUE(extractOne(true));
            EXPECT_FALSE(extractOne(false));
        }

        /// A dead slot keeps the position its last particle expired at, and an emitter with nothing
        /// alive places nothing at all — not a sphere with an empty run behind it, which every ray
        /// crossing that part of the cell would then be rejected by one test later than it needs.
        TEST_F(RtxSceneExtractorTest, deadParticlesAndUntexturedEmittersPlaceNothing)
        {
            const Plume spent = makePlume(osg::Matrix::identity(), true);
            osgParticle::Particle* particle
                = emit(*spent.mParticles, osg::Vec3f(0.0f, 0.0f, 5.0f), 3.0f, osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f));
            particle->kill();
            particle->update(0.0, false);
            ASSERT_FALSE(particle->isAlive());

            const ExtractionStats stats = walk(*spent.mRoot);

            EXPECT_EQ(stats.mEmitters, 0u);
            EXPECT_EQ(stats.mSprites, 0u);
            EXPECT_TRUE(mScene.getEmitters().empty());

            // The texture is registered the moment the emitter is met, alive or not: it is what the
            // array is built from, and one that turns up two hundred frames later has nowhere to go.
            // The bake of its alpha arrives with it, for the same reason.
            EXPECT_EQ(mScene.getTextures().size(), 2u);

            // A particle's whole silhouette is that texture's alpha, so an emitter with none draws
            // nothing rather than a white disc.
            osg::ref_ptr<osg::Group> bare = new osg::Group;
            osg::ref_ptr<osgParticle::ParticleSystem> particles = new osgParticle::ParticleSystem;
            bare->addChild(particles);
            emit(*particles, osg::Vec3f(), 1.0f, osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f));

            Rtx::SceneDesc bareScene;
            SceneExtractor bareExtractor(bareScene);
            EXPECT_EQ(bareExtractor.extract(*bare, osg::Matrixf::identity(), 0).mEmitters, 0u);
            EXPECT_TRUE(bareScene.getTextures().empty());
        }

        /// An emitter's sprite is on no material, so the sweep has to speak for it itself.
        ///
        /// The emitter outlives a textured quad here, and the texture the quad wore is what proves
        /// the sweep is doing anything at all: a pass that kept every texture would keep both.
        TEST_F(RtxSceneExtractorTest, aSweepKeepsTheTextureAnEmitterIsStillDrawingWith)
        {
            osg::ref_ptr<osg::Geometry> stone = makeQuad();
            paint(*stone->getOrCreateStateSet(), "textures/tx_stone_01.dds");

            const Plume plume = makePlume(osg::Matrix::identity(), /*additive=*/true);
            emit(*plume.mParticles, osg::Vec3f(), 1.0f, osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f));

            osg::ref_ptr<osg::Group> both = new osg::Group;
            both->addChild(stone);
            both->addChild(plume.mRoot);

            walk(*both);
            ASSERT_EQ(mScene.getTextures().size(), 3u) << "the stone's, the sprite's and the sprite's bake";
            ASSERT_TRUE(mExtractor.retire().empty());

            mScene.clearPlacement();
            walk(*plume.mRoot);

            const Retirement went = mExtractor.retire();

            EXPECT_EQ(went.mMeshes, 1u) << "the stone the second walk did not meet";
            EXPECT_EQ(went.mMaterials, 1u);

            // No slot is reclaimed from the table — nothing is renumbered — so what this asserts is
            // that the sprite's texture is still *named*, which is the thing the emitter map exists
            // for: a sprite hangs off no material, so nothing else holds it. The stone's went with
            // the stone's material, which is the other half of the same statement.
            ASSERT_EQ(mScene.getTextures().size(), 3u);
            EXPECT_TRUE(mScene.getTextures()[0].value().empty()) << "the stone's texture outlived the stone";
            EXPECT_EQ(mScene.getTextures()[1], VFS::Path::NormalizedView("textures/tx_fire_00.dds"));
            EXPECT_FALSE(mScene.getBakedTextures()[2].empty()) << "the sprite's bake went with the stone";

            // And the emitter still draws with it.
            mScene.clearPlacement();
            walk(*plume.mRoot);

            ASSERT_EQ(mScene.getEmitters().size(), 1u);
            EXPECT_EQ(mScene.getEmitters().front().mTexture, 1u) << "the sprite lost the slot it was given";
            EXPECT_EQ(mScene.getEmitters().front().mLighting, 2u) << "the bake lost the slot it was given";
            EXPECT_EQ(mScene.getTextures().size(), 3u) << "the sprite's path was added a second time";

            // **And the other way round, on the frame the sweep does not look at.** The stone comes
            // back and then the emitter goes, taking no mesh and no material with it — which is
            // exactly the frame `SceneDesc::release` answers with two comparisons and returns from.
            // The sprite's slot has to be given back by whatever was holding it.
            mScene.clearPlacement();
            walk(*both);
            ASSERT_TRUE(mExtractor.retire().empty());
            ASSERT_EQ(mScene.getTextures()[0], VFS::Path::NormalizedView("textures/tx_stone_01.dds"));

            mScene.clearPlacement();
            walk(*stone);

            EXPECT_TRUE(mExtractor.retire().empty()) << "an emitter is neither a mesh nor a material";
            EXPECT_TRUE(mScene.getTextures()[1].value().empty()) << "the sprite outlived the emitter";
            EXPECT_TRUE(mScene.getBakedTextures()[2].empty()) << "the bake outlived the emitter";
        }

        /// Gives a plume what makes it run: something emitting at a fixed rate, and the updater
        /// that integrates what it emitted.
        ///
        /// Both go in above the system they drive, which is where `NifOsg` puts them and where
        /// `osgParticle` needs them — a walk that meets the updater first reads a system already
        /// integrated this frame rather than one frame of staleness.
        ///
        /// Frozen on cull to start with, because that is how a system arrives from a file and how a
        /// system that has never been drawn stays: `_last_frame` moves in `drawImplementation` and
        /// nowhere else, so nothing here would ever advance it.
        void drive(Plume& plume, double perSecond, bool updaterAbove = true)
        {
            plume.mParticles->setFreezeOnCull(true);

            osgParticle::Particle& seed = plume.mParticles->getDefaultParticleTemplate();
            seed.setLifeTime(10.0f);
            seed.setSizeRange(osgParticle::rangef(2.0f, 2.0f));
            seed.setAlphaRange(osgParticle::rangef(1.0f, 1.0f));
            seed.setColorRange(
                osgParticle::rangev4(osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f), osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f)));

            osg::ref_ptr<osgParticle::ConstantRateCounter> counter = new osgParticle::ConstantRateCounter;
            counter->setNumberOfParticlesPerSecondToCreate(perSecond);

            // Straight up at a fixed speed, so that where a particle has got to is a number this
            // test can compare rather than a random direction.
            osg::ref_ptr<osgParticle::RadialShooter> shooter = new osgParticle::RadialShooter;
            shooter->setThetaRange(0.0f, 0.0f);
            shooter->setPhiRange(0.0f, 0.0f);
            shooter->setInitialSpeedRange(100.0f, 100.0f);
            shooter->setInitialRotationalSpeedRange(osg::Vec3f(), osg::Vec3f());

            osg::ref_ptr<osgParticle::ModularEmitter> emitter = new osgParticle::ModularEmitter;
            emitter->setParticleSystem(plume.mParticles);
            emitter->setCounter(counter);
            emitter->setShooter(shooter);

            osg::ref_ptr<osgParticle::ParticleSystemUpdater> updater = new osgParticle::ParticleSystemUpdater;
            updater->addParticleSystem(plume.mParticles);

            plume.mRoot->insertChild(0, emitter);
            if (updaterAbove)
                plume.mRoot->insertChild(1, updater);
            else
                plume.mRoot->addChild(updater);
        }

        /// Every sprite the scene holds, by height.
        std::vector<float> spriteHeights(const Rtx::SceneDesc& scene)
        {
            std::vector<float> heights;
            for (const Rtx::Sprite& sprite : scene.getSprites())
                heights.push_back(sprite.mPosition.z());

            std::sort(heights.begin(), heights.end());
            return heights;
        }

        /// Where an emitter's particles have got to does not depend on where its updater sits.
        ///
        /// **`osgParticle` splits emission from integration across two sibling nodes**, so a walk
        /// that reads the particles as it passes them reads a different frame's worth depending on
        /// which sibling comes first. `NifOsg` puts the updater above the system deliberately and
        /// every model in the game obeys that, which makes the dependency invisible right up until
        /// something hand-built does not — and then it is one frame of staleness in a position,
        /// which nothing will ever notice. Reading after the walk has settled removes the question,
        /// and this is the assertion that says so.
        TEST_F(RtxSceneExtractorTest, spritesAreReadAfterTheWalkRatherThanAsItPassesThem)
        {
            const auto run = [](bool updaterAbove) {
                resetRandom();

                Plume plume = makePlume(osg::Matrix::identity(), /*additive=*/true);
                drive(plume, 100.0, updaterAbove);

                Rtx::SceneDesc scene;
                SceneExtractor extractor(scene);

                // The first turn only starts the clock; the second emits and integrates.
                for (int turn = 0; turn < 2; ++turn)
                {
                    scene.clearPlacement();
                    extractor.advanceEmitters(0.1);
                    extractor.extract(*plume.mRoot, osg::Matrixf::identity(), 0);
                }

                return spriteHeights(scene);
            };

            const std::vector<float> above = run(true);
            const std::vector<float> below = run(false);

            ASSERT_EQ(above.size(), 10u);
            EXPECT_EQ(above, below) << "the graph's order decided what a sprite's position was";

            // **And they have actually moved**, so that the agreement above is two settled reads
            // rather than two stale ones. A particle shot straight up at a hundred units a second
            // is at least a whole tenth-second step off the placer's origin, which is ten.
            EXPECT_GE(above.front(), 10.0f);
        }

        /// An emitter runs, and it runs once per turn of the emitter clock however often it is walked.
        ///
        /// **`osgParticle` hangs its whole simulation off the cull traversal**, and a ray tracer has
        /// none — so without the walk claiming that name at the two nodes that ask, every particle
        /// system in the world stands still on the seed its file was authored with. That failure is
        /// silent: the scene still has an emitter in it and still places sprites, they just never
        /// change, which is why this asserts on a count that moves rather than on one that exists.
        TEST_F(RtxSceneExtractorTest, anEmitterRunsOnTheEmitterClockAndOnlyOncePerTurnOfIt)
        {
            Plume plume = makePlume(osg::Matrix::identity(), /*additive=*/true);
            drive(plume, 100.0);

            // **The first turn only starts the clock.** `ParticleProcessor` keeps the last time it
            // saw and has none yet, so it records one and steps nothing — which is also why a
            // renderer that walks a cell once and shows it has to warm its emitters first.
            mExtractor.advanceEmitters(0.1);
            EXPECT_EQ(walk(*plume.mRoot).mSprites, 0u);

            EXPECT_FALSE(plume.mParticles->getFreezeOnCull())
                << "freeze-on-cull asks whether the draw has touched this, and nothing here draws";

            // A tenth of a second at a hundred a second is ten.
            mExtractor.advanceEmitters(0.1);
            EXPECT_EQ(walk(*plume.mRoot).mSprites, 10u);

            // **The same ten, and not another ten.** Every walk that reaches an emitter says it is a
            // cull traversal, and the game reaches this one twice a frame — the world's walk and the
            // weather's. What keeps that one step is `ParticleProcessor`'s own once-per-frame guard,
            // and it only holds while a single clock is writing it.
            EXPECT_EQ(walk(*plume.mRoot).mSprites, 10u);

            // Ten more on the next turn, so the guard is a guard and not a stop.
            mExtractor.advanceEmitters(0.1);
            EXPECT_EQ(walk(*plume.mRoot).mSprites, 20u);
        }

        /// A gap in the world's clock is clamped rather than emitted.
        ///
        /// A loading screen, a paused window or a harness holding the world still are each a gap an
        /// emitter would take literally, and a literal hour at a hundred a second is three hundred
        /// and sixty thousand particles in one frame.
        TEST_F(RtxSceneExtractorTest, aJumpInTheWorldsClockIsClampedRatherThanEmitted)
        {
            Plume plume = makePlume(osg::Matrix::identity(), /*additive=*/true);
            drive(plume, 100.0);

            mExtractor.advanceEmitters(0.1);
            walk(*plume.mRoot);

            // Clamped to the two tenths the game's own frame loop caps a step at: twenty, not
            // 360,000.
            mExtractor.advanceEmitters(3600.0);
            EXPECT_EQ(walk(*plume.mRoot).mSprites, 20u);

            // And a step backwards is not a step backwards, it is no step at all.
            mExtractor.advanceEmitters(-10.0);
            EXPECT_EQ(walk(*plume.mRoot).mSprites, 20u);
        }

        /// The emitters can be run without a frame being mirrored, which is what a warm-up is.
        TEST_F(RtxSceneExtractorTest, emittersCanBeSteppedWithoutMirroringAnything)
        {
            Plume plume = makePlume(osg::Matrix::identity(), /*additive=*/true);
            drive(plume, 100.0);

            for (int turn = 0; turn < 3; ++turn)
            {
                mExtractor.advanceEmitters(0.1);
                mExtractor.stepEmitters(*plume.mRoot);
            }

            EXPECT_EQ(mScene.getSprites().size(), 0u) << "stepping is not mirroring: nothing was placed";

            // Two of those three turns emitted — the first only started the clock — and the walk that
            // finally mirrors them adds none of its own, because its turn is already spent.
            EXPECT_EQ(walk(*plume.mRoot).mSprites, 20u);
        }
    }
}
