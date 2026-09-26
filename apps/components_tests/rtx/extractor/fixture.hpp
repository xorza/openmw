#pragma once

#include <cstddef>
#include <cstdlib>
#include <source_location>
#include <vector>

#include <gtest/gtest.h>

#include <osg/BlendFunc>
#include <osg/GL>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Image>
#include <osg/Matrix>
#include <osg/MatrixTransform>
#include <osg/Matrixf>
#include <osg/Node>
#include <osg/StateAttribute>
#include <osg/Vec3f>
#include <osg/Vec4f>
#include <osg/ref_ptr>
#include <osgParticle/Particle>
#include <osgParticle/ParticleSystem>
#include <osgParticle/range>
#include <osgUtil/UpdateVisitor>

#include <components/nifosg/nifloader.hpp>
#include <components/rtx/extractionstats.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>
#include <components/sceneutil/lightutil.hpp>
#include <components/sceneutil/material.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/skeleton.hpp>
#include <components/sceneutil/statesetupdater.hpp>

#include "../support/graph.hpp"

namespace Rtx::Testing
{
    /// A particle system under a transform that carries its texture, its blend and its
    /// material, the way `NifOsg` builds one: the material reads the vertex for its colour,
    /// which is what the loader gives every particle system a file does not say otherwise for.
    ///
    /// The emitter's own state set sets none of them, which is what makes this a test of the
    /// walk up the path rather than of the drawable: a `ParticleSystem` really does carry an
    /// empty state set of its own in the shipped content, and asking it for the blend answers
    /// "covers" for every flame in the game.
    struct Plume
    {
        osg::ref_ptr<osg::MatrixTransform> mRoot;
        osg::ref_ptr<osgParticle::ParticleSystem> mParticles;
    };

    /// @param sprite the texture, or null for one that is nothing but a name, which is all a walk
    ///        reads of a sprite unless an effect's lamp asks what its texels average.
    inline Plume makePlume(const osg::Matrix& place, bool additive, osg::Image* sprite = nullptr)
    {
        Plume plume;
        plume.mRoot = new osg::MatrixTransform(place);

        osg::StateSet& state = *plume.mRoot->getOrCreateStateSet();
        if (sprite != nullptr)
            paint(state, *sprite);
        else
            paint(state, "textures/tx_fire_00.dds");
        state.setAttributeAndModes(new osg::BlendFunc(osg::BlendFunc::SRC_ALPHA,
                                       additive ? osg::BlendFunc::ONE : osg::BlendFunc::ONE_MINUS_SRC_ALPHA),
            osg::StateAttribute::ON);
        colours(state).setVertexColorMode(SceneUtil::VertexColorModes::AmbientAndDiffuse);

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
    inline osgParticle::Particle* emit(
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

    /// Puts the shared random sequence back where it started.
    ///
    /// **`osgParticle` draws from `std::rand` for every range it reads**, the single-value ones
    /// these fixtures set included — so a plume run from two different points in that sequence is
    /// two different plumes, and a test comparing the two measures the sequence rather than what it
    /// meant to. The harness's `--random-seed` (`hosted.cpp`) is the same problem where a whole run
    /// is compared with another, with the driver drawing from the sequence too; here the difference
    /// measured was one unit in the last place of a height.
    ///
    /// **Called per run and not once per test, which is why no fixture set-up does it.** The first
    /// run advances the sequence, so a reset at the top of the test would leave the second run
    /// starting somewhere else. Which point the sequence is put back to does not matter, only that
    /// both runs get the same one.
    inline void resetRandom()
    {
        std::srand(1);
    }

    /// The scene every test here fills, and the walk that fills it.
    ///
    /// **Held by the fixture rather than opened by each test**, because the pair is what a test of
    /// the extractor is made of, and the transform and the anchor a walk takes are the same in
    /// nearly all of them. A test that wants a second scene, or a fresh walk on every call of a
    /// lambda, still builds its own — and says something by doing it.
    class RtxSceneExtractorTest : public ::testing::Test
    {
    protected:
        /// Walks `node` into `mScene` from the world's own origin.
        ///
        /// @param anchor what the walk is placing, which only a test that places two things through
        ///        one walk has to tell apart.
        /// @param frame the game's own, which only a test about a semi-active skeleton needs.
        ExtractionStats walk(const osg::Node& node, std::size_t anchor = 0, std::size_t frame = 0)
        {
            return mExtractor.extract(node, osg::Matrixf::identity(), anchor, frame);
        }

        Rtx::SceneDesc mScene;
        SceneExtractor mExtractor{ mScene };
    };

    /// Where the walk put instance `index`: the origin of its own space, carried through the
    /// transform the walk gave it.
    inline osg::Vec3f placedAt(const Rtx::SceneDesc& scene, std::size_t index)
    {
        return osg::Vec3f() * scene.placements().getRows()[index].mInstance.mTransform;
    }

    /// A skeleton with one bone, and a rig bound rigidly to it.
    ///
    /// Every weight on one bone with an identity bind matrix makes the skinning arithmetic the
    /// bone's own transform and nothing else, so what a test expects is what it moved the bone
    /// by — rather than a fit against whatever a weighted sum happened to produce.
    struct RiggedQuad
    {
        osg::ref_ptr<SceneUtil::Skeleton> mSkeleton = new SceneUtil::Skeleton;
        osg::ref_ptr<osg::MatrixTransform> mBone = new osg::MatrixTransform;
        osg::ref_ptr<SceneUtil::RigGeometry> mRig = new SceneUtil::RigGeometry;
        osg::ref_ptr<osg::Geometry> mSource = makeQuad();

        RiggedQuad()
        {
            mBone->setName("bone");
            mSkeleton->addChild(mBone);

            mRig->setName("shape");
            mRig->setBoneInfo({ SceneUtil::RigGeometry::BoneInfo{
                .mName = "bone", .mBoundSphere = {}, .mInvBindMatrix = osg::Matrixf::identity() } });
            mRig->setInfluences(std::vector<SceneUtil::RigGeometry::BoneWeights>(
                4, SceneUtil::RigGeometry::BoneWeights{ { 0, 1.0f } }));
            mRig->setSourceGeometry(mSource);

            // **A named shape one level below the skeleton, which is the shape NIF content
            // has.** `RigGeometry::updateSkinToSkelMatrix` reads the node path backwards from
            // the trishape's own transform, and a rig hung straight off the skeleton walks off
            // the front of it.
            osg::ref_ptr<osg::Group> holder = new osg::Group;
            holder->addChild(mRig);
            mSkeleton->addChild(holder);
        }

        /// The update traversal the game runs before it mirrors anything.
        ///
        /// **`RigGeometry` finds its skeleton here and nowhere else.** It walks the node path
        /// for one, and the pose traversal the mirror uses is handed the drawable on its own —
        /// so a rig that had never been through an update would have nothing to skin against.
        void update(unsigned int traversal)
        {
            osgUtil::UpdateVisitor visitor;
            visitor.setTraversalNumber(traversal);
            mSkeleton->accept(visitor);
        }
    };

    /// A state-set controller of the shape `NifOsg` builds out of a `NiMaterialColorController`:
    /// it rewrites one attribute every time it is applied, and it hangs from whichever callback
    /// chain the content asked for.
    class ColourController : public SceneUtil::StateSetUpdater
    {
    public:
        float mRed = 0.0f;

        /// A texture the controlled surface wears, for the tests that care what an animated
        /// material is read from. Null leaves the surface untextured, which is what most of them
        /// want.
        osg::ref_ptr<osg::Image> mDiffuse;

        void setDefaults(osg::StateSet* stateset) override
        {
            stateset->setAttribute(new SceneUtil::Material, osg::StateAttribute::ON);

            if (mDiffuse != nullptr)
                paint(*stateset, *mDiffuse);
        }

        void apply(osg::StateSet* stateset, osg::NodeVisitor*) override
        {
            colours(*stateset).setDiffuse(osg::Vec4f(mRed, 0.0f, 0.0f, 1.0f));
        }
    };

    /// What a `ColourController`'s red comes to in the scene: the sRGB curve divided out of it, and
    /// nothing at all in the other two channels.
    ///
    /// **Within a millionth in the one channel that moves**, because the curve is a `pow` and a
    /// test states what it expects as a decimal. The other two are exact, since nought decodes to
    /// nought — and they are asked because a decode that swapped the channels would pass on the
    /// first alone.
    ///
    /// @param where the caller's line, so a failure reports there rather than here.
    inline void expectRed(
        const osg::Vec3f& colour, float red, std::source_location where = std::source_location::current())
    {
        const ::testing::ScopedTrace trace(where.file_name(), static_cast<int>(where.line()), "expectRed");

        EXPECT_NEAR(colour.x(), red, 1e-6f);
        EXPECT_EQ(colour.y(), 0.0f);
        EXPECT_EQ(colour.z(), 0.0f);
    }
}
