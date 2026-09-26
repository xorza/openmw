#include <cstddef>
#include <initializer_list>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include <osg/Array>
#include <osg/Drawable>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/LOD>
#include <osg/Material>
#include <osg/Matrix>
#include <osg/MatrixTransform>
#include <osg/Matrixf>
#include <osg/Node>
#include <osg/Sequence>
#include <osg/StateSet>
#include <osg/Switch>
#include <osg/Vec3f>
#include <osg/Vec4f>
#include <osg/ref_ptr>

#include <components/rtx/meshreader.hpp>
#include <components/rtx/meshtable.hpp>
#include <components/rtx/prepared.hpp>
#include <components/rtx/result.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/surface.hpp>
#include <components/rtx/templatewalk.hpp>
#include <components/shader/automaps.hpp>

#include "support/graph.hpp"

namespace Rtx::Testing
{
    namespace
    {
        /// The walk reaches what the frame's walk reaches of a model that stands still, and hands
        /// each drawable the chain and the transform the frame would have composed for it.
        TEST(RtxTemplateWalkTest, aTemplateIsWalkedByTheFrameWalksRulesForWhatStandsStill)
        {
            osg::ref_ptr<osg::Group> root = new osg::Group;
            osg::StateSet* rootState = root->getOrCreateStateSet();

            // A colour on the root, so a part read under it shows the root's state set was in
            // force at the drawable.
            osg::ref_ptr<osg::Material> tint = new osg::Material;
            tint->setDiffuse(osg::Material::FRONT_AND_BACK, osg::Vec4f(0.25f, 0.5f, 0.75f, 1.0f));
            rootState->setAttribute(tint);

            // Ten units along x, over everything below.
            osg::ref_ptr<osg::MatrixTransform> moved
                = new osg::MatrixTransform(osg::Matrix::translate(10.0f, 0.0f, 0.0f));
            root->addChild(moved);

            // A switch with its first branch off: the harvested plant, the night lamp at noon.
            osg::ref_ptr<osg::Switch> branches = new osg::Switch;
            osg::ref_ptr<osg::Geometry> off = makeQuad();
            osg::ref_ptr<osg::Geometry> on = makeQuad();
            osg::StateSet* onState = on->getOrCreateStateSet();
            branches->addChild(off, false);
            branches->addChild(on, true);
            moved->addChild(branches);

            // A flipbook standing on its second frame.
            osg::ref_ptr<osg::Sequence> frames = new osg::Sequence;
            osg::ref_ptr<osg::Geometry> first = makeQuad();
            osg::ref_ptr<osg::Geometry> second = makeQuad();
            frames->addChild(first);
            frames->addChild(second);
            frames->setValue(1);
            moved->addChild(frames);

            // An LOD stands its nearest level, the far one first so that the answer is not the
            // first child: a ray is owed the finest, as the frame walk takes it.
            osg::ref_ptr<osg::LOD> levels = new osg::LOD;
            osg::ref_ptr<osg::Geometry> near = makeQuad();
            osg::ref_ptr<osg::Geometry> far = makeQuad();
            levels->addChild(far, 100.0f, 1000.0f);
            levels->addChild(near, 0.0f, 100.0f);
            moved->addChild(levels);

            // The near level carries tangents, and the only ones: its part's run of them starts the
            // model's.
            osg::ref_ptr<osg::Vec4Array> tangents = new osg::Vec4Array;
            for (const float handedness : { 1.0f, -1.0f, 1.0f, -1.0f })
                tangents->push_back(osg::Vec4f(1.0f, 0.0f, 0.0f, handedness));
            near->setTexCoordArray(Shader::sTangentUnit, tangents, osg::Array::BIND_PER_VERTEX);

            // What the loader hid, under a mask the walk is told to keep out of.
            constexpr osg::Node::NodeMask hidden = 0x1;
            osg::ref_ptr<osg::Geometry> collision = makeQuad();
            collision->setNodeMask(hidden);
            root->addChild(collision);

            PreparedModel model;
            TemplateWalk walk;
            walk.read(*root, ~hidden, model);

            ASSERT_EQ(model.mParts.size(), 3u) << "the branch that is on, the frame shown, and the near level";
            EXPECT_EQ(model.mPositions.size(), 12u) << "three quads' corners, appended in turn";

            EXPECT_EQ(model.mParts[0].mDrawable, on.get());
            EXPECT_EQ(model.mParts[0].mMaterial.mKey, onState) << "held under the drawable's own state set";
            ASSERT_TRUE(model.mParts[0].mMaterial.mDescribed.has_value());
            EXPECT_EQ(model.mParts[0].mMaterial.mDescribed->mDiffuseColour, (EncodedColour{ 0.25f, 0.5f, 0.75f }))
                << "and the root's state set was in force at it";
            EXPECT_EQ(osg::Vec3f() * model.mParts[0].mLocal, osg::Vec3f(10.0f, 0.0f, 0.0f))
                << "moved by the transform above it";
            EXPECT_EQ(model.mParts[0].mVertices, (Rtx::Run{ .mOffset = 0, .mCount = 4 }));

            EXPECT_EQ(model.mParts[1].mDrawable, second.get()) << "the frame the sequence stands on, unstepped";
            EXPECT_EQ(model.mParts[1].mMaterial.mKey, rootState) << "nearest last, and the root is all there is";
            EXPECT_EQ(model.mParts[1].mVertices, (Rtx::Run{ .mOffset = 4, .mCount = 4 }));

            EXPECT_EQ(model.mParts[2].mDrawable, near.get());
            EXPECT_EQ(model.mParts[0].mTangents.mCount, 0u);
            EXPECT_EQ(model.mParts[1].mTangents.mCount, 0u);
            EXPECT_EQ(model.mParts[2].mTangents, (Rtx::Run{ .mOffset = 0, .mCount = 4 }));
            const MeshReading nearReading = model.readingOf(model.mParts[2]);
            ASSERT_EQ(nearReading.mArrays.mTangents.size(), 4u);
            for (std::size_t vertex = 0; vertex < 4; ++vertex)
                EXPECT_EQ(nearReading.mArrays.mTangents[vertex], (*tangents)[vertex]) << vertex;

            for (const PreparedPart& part : model.mParts)
            {
                EXPECT_NE(part.mDrawable, off.get()) << "a branch that is off is not in the world";
                EXPECT_NE(part.mDrawable, first.get()) << "a frame the flipbook is not on is not shown";
                EXPECT_NE(part.mDrawable, far.get()) << "a level for a farther eye is not the finest";
                EXPECT_NE(part.mDrawable, collision.get()) << "what the loader hid is not walked";
            }

            // **Nothing was stepped.** A frame's walk moves a flipbook's clock; this one may not,
            // because the template is every clone's and every thread's.
            EXPECT_EQ(frames->getValue(), 1);
        }

        /// **A mesh this cannot build refuses its model on the reader's thread**, where the reader
        /// leaves the model out of its cell. Left to the adoption, the same check refused inside
        /// the frame's walk. A mesh past one block is one, and a triangle naming a vertex its
        /// drawable does not have is another, and the walk says which. The walk after it is the
        /// next model's, whole.
        TEST(RtxTemplateWalkTest, aMeshThisCannotBuildRefusesTheModelWhereItIsRead)
        {
            const std::string pastABlock = "its " + std::to_string(MeshTable::sVertexBlock + 1)
                + " vertices and 3 indices are past the " + std::to_string(MeshTable::sVertexBlock) + " and "
                + std::to_string(MeshTable::sIndexBlock) + " one block of the shared buffers holds";

            TemplateWalk walk;
            for (const auto& [broken, why] : { std::pair{ makePastOneBlock(), pastABlock },
                     std::pair{ makeIndexPastItsVertices(), std::string("its triangles name vertex 4 of 4") } })
            {
                osg::ref_ptr<osg::Group> root = new osg::Group;
                root->addChild(makeQuad());
                root->addChild(broken);

                PreparedModel model;
                const Result<void, std::string> refused = walk.read(*root, ~0u, model);
                ASSERT_FALSE(refused.isOk());
                EXPECT_EQ(refused.error(), why);

                const osg::ref_ptr<osg::Geometry> quad = makeQuad();
                PreparedModel next;
                EXPECT_TRUE(walk.read(*quad, ~0u, next).isOk()) << "a refusal carried into the next model";
                EXPECT_EQ(next.mParts.size(), 1u);
            }
        }
    }
}
