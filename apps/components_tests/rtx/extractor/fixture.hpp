#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <source_location>

#include <gtest/gtest.h>

#include <osg/BlendFunc>
#include <osg/CullFace>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Image>
#include <osg/Material>
#include <osg/MatrixTransform>
#include <osg/Matrixf>
#include <osg/Node>
#include <osg/Sequence>
#include <osg/Switch>
#include <osg/Texture2D>
#include <osg/observer_ptr>
#include <osgParticle/ConstantRateCounter>
#include <osgParticle/ModularEmitter>
#include <osgParticle/Particle>
#include <osgParticle/ParticleSystem>
#include <osgParticle/ParticleSystemUpdater>
#include <osgParticle/RadialShooter>
#include <osgUtil/UpdateVisitor>

#include <components/esm3/loadligh.hpp>
#include <components/nifosg/nifloader.hpp>
#include <components/rtx/extractionstats.hpp>
#include <components/rtx/instancerecord.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/spritelight.hpp>
#include <components/sceneutil/lightcommon.hpp>
#include <components/sceneutil/lightcontroller.hpp>
#include <components/sceneutil/lightutil.hpp>
#include <components/sceneutil/material.hpp>
#include <components/sceneutil/morphgeometry.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/skeleton.hpp>
#include <components/sceneutil/statesetupdater.hpp>
#include <components/sceneutil/texturetype.hpp>
#include <components/sceneutil/vismask.hpp>
#include <components/surface/material.hpp>

#include "../graphlight.hpp"

namespace Rtx::Testing
{
    inline osg::ref_ptr<osg::Vec3Array> makePositions(std::initializer_list<osg::Vec3f> values)
    {
        osg::ref_ptr<osg::Vec3Array> positions = new osg::Vec3Array;
        for (const osg::Vec3f& value : values)
            positions->push_back(value);
        return positions;
    }

    inline osg::ref_ptr<osg::DrawElementsUInt> makeTriangles(std::initializer_list<unsigned int> indices)
    {
        osg::ref_ptr<osg::DrawElementsUInt> triangles = new osg::DrawElementsUInt(osg::PrimitiveSet::TRIANGLES);
        for (const unsigned int index : indices)
            triangles->push_back(index);
        return triangles;
    }

    /// The material attribute on a state set, made where nothing has set one yet.
    ///
    /// Every fixture here stands in for something `NifOsg` built, and a walk reads what that built
    /// off the state set — so a colour a test wants read goes on the attribute the loader would
    /// have put it on.
    inline SceneUtil::Material& colours(osg::StateSet& state)
    {
        auto* material = dynamic_cast<SceneUtil::Material*>(state.getAttribute(osg::StateAttribute::MATERIAL));
        if (material == nullptr)
        {
            material = new SceneUtil::Material;
            state.setAttribute(material, osg::StateAttribute::ON);
        }

        return *material;
    }

    /// Binds a texture the way a loader does: on the next free unit, with the type beside it that
    /// names its role.
    inline void paint(
        osg::StateSet& state, osg::Image& image, Surface::TextureRole role = Surface::TextureRole::Diffuse)
    {
        const unsigned int unit = static_cast<unsigned int>(state.getTextureAttributeList().size());
        state.setTextureAttributeAndModes(unit, new osg::Texture2D(&image), osg::StateAttribute::ON);
        state.setTextureAttribute(
            unit, new SceneUtil::TextureType(std::string(Surface::textureRoleName(role))), osg::StateAttribute::ON);
    }

    /// The same for a texture that is nothing but a name, which is all a walk reads of most of
    /// them.
    inline void paint(
        osg::StateSet& state, std::string_view file, Surface::TextureRole role = Surface::TextureRole::Diffuse)
    {
        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->setFileName(std::string(file));

        paint(state, *image, role);
    }

    /// Puts the shared random sequence back where it started.
    ///
    /// **`osgParticle` draws from `std::rand` for every range it reads**, the single-value ones these
    /// fixtures set included — so a plume run from two different points in that sequence is two
    /// different plumes, and a test comparing the two measures the sequence rather than what it
    /// meant to. `RtxTool::StagedWorld::seedDraws` is the same problem where a whole world is
    /// staged twice; here the difference measured was one unit in the last place of a height.
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
        return osg::Vec3f() * scene.getTables().mPlacements.getAll()[index].mTransform;
    }

    /// A unit quad in the xy plane: four vertices, two triangles.
    inline osg::ref_ptr<osg::Geometry> makeQuad()
    {
        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
        geometry->setVertexArray(makePositions({
            osg::Vec3f(0.0f, 0.0f, 0.0f),
            osg::Vec3f(1.0f, 0.0f, 0.0f),
            osg::Vec3f(1.0f, 1.0f, 0.0f),
            osg::Vec3f(0.0f, 1.0f, 0.0f),
        }));
        geometry->addPrimitiveSet(makeTriangles({ 0, 1, 2, 0, 2, 3 }));
        return geometry;
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
