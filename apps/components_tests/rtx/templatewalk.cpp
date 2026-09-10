#include <cstddef>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Drawable>
#include <osg/Group>
#include <osg/LOD>
#include <osg/MatrixTransform>
#include <osg/Matrixf>
#include <osg/Sequence>
#include <osg/StateSet>
#include <osg/Switch>
#include <osg/Vec3f>

#include <components/rtx/shading.hpp>
#include <components/rtx/templatewalk.hpp>

#include "extractor/fixture.hpp"

namespace Rtx::Testing
{
    namespace
    {
        /// What one drawable arrived as.
        struct Taken
        {
            const osg::Drawable* mDrawable = nullptr;
            std::vector<const osg::StateSet*> mChain;
            osg::Vec3f mOrigin;
        };

        struct Record final : TemplateSink
        {
            std::vector<Taken> mTaken;

            void take(
                const osg::Drawable& drawable, std::span<const Shading> shading, const osg::Matrixf& local) override
            {
                Taken taken{ .mDrawable = &drawable, .mOrigin = osg::Vec3f() * local };
                for (const Shading& link : shading)
                    taken.mChain.push_back(link.mStateSet);
                mTaken.push_back(std::move(taken));
            }
        };

        /// The walk reaches what the frame's walk reaches of a model that stands still, and hands
        /// each drawable the chain and the transform the frame would have composed for it.
        TEST(RtxTemplateWalkTest, aTemplateIsWalkedByTheFrameWalksRulesForWhatStandsStill)
        {
            osg::ref_ptr<osg::Group> root = new osg::Group;
            osg::StateSet* rootState = root->getOrCreateStateSet();

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

            // An LOD is walked whole: a ray is owed the finest child, and the frame walk takes all.
            osg::ref_ptr<osg::LOD> levels = new osg::LOD;
            osg::ref_ptr<osg::Geometry> near = makeQuad();
            osg::ref_ptr<osg::Geometry> far = makeQuad();
            levels->addChild(near, 0.0f, 100.0f);
            levels->addChild(far, 100.0f, 1000.0f);
            moved->addChild(levels);

            // What the loader hid, under a mask the walk is told to keep out of.
            constexpr osg::Node::NodeMask hidden = 0x1;
            osg::ref_ptr<osg::Geometry> collision = makeQuad();
            collision->setNodeMask(hidden);
            root->addChild(collision);

            Record record;
            TemplateWalk walk;
            walk.walk(*root, ~hidden, record);

            ASSERT_EQ(record.mTaken.size(), 4u) << "the branch that is on, the frame shown, and both levels";

            EXPECT_EQ(record.mTaken[0].mDrawable, on.get());
            EXPECT_EQ(record.mTaken[0].mChain, (std::vector<const osg::StateSet*>{ rootState, onState }))
                << "the root's state set and the drawable's own, nearest last";
            EXPECT_EQ(record.mTaken[0].mOrigin, osg::Vec3f(10.0f, 0.0f, 0.0f)) << "moved by the transform above it";

            EXPECT_EQ(record.mTaken[1].mDrawable, second.get()) << "the frame the sequence stands on, unstepped";
            EXPECT_EQ(record.mTaken[1].mChain, std::vector<const osg::StateSet*>{ rootState });

            EXPECT_EQ(record.mTaken[2].mDrawable, near.get());
            EXPECT_EQ(record.mTaken[3].mDrawable, far.get());

            for (const Taken& taken : record.mTaken)
            {
                EXPECT_NE(taken.mDrawable, off.get()) << "a branch that is off is not in the world";
                EXPECT_NE(taken.mDrawable, first.get()) << "a frame the flipbook is not on is not shown";
                EXPECT_NE(taken.mDrawable, collision.get()) << "what the loader hid is not walked";
            }

            // **Nothing was stepped.** A frame's walk moves a flipbook's clock; this one may not,
            // because the template is every clone's and every thread's.
            EXPECT_EQ(frames->getValue(), 1);
        }
    }
}
