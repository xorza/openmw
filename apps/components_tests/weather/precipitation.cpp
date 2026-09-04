#include <cstddef>

#include <gtest/gtest.h>

#include <osg/Group>
#include <osg/MatrixTransform>
#include <osg/NodeVisitor>
#include <osg/PositionAttitudeTransform>
#include <osg/Transform>

#include <components/weather/precipitation.hpp>

#include "../rtx/allocations.hpp"

namespace Weather
{
    namespace
    {
        /// A transform that answers about the visitor it was handed rather than about a frame.
        ///
        /// **What `MWRender::CameraRelativeTransform` does, restated where this file can reach it.**
        /// That one reads the visitor's type as its first statement and casts it to
        /// `osgUtil::CullVisitor` where the type says cull. It lives in the rasterizer, so a test
        /// cannot include it, and what is asserted here is the half of its contract the walk owes.
        class RecordsTheVisitor : public osg::Transform
        {
        public:
            bool computeLocalToWorldMatrix(osg::Matrix&, osg::NodeVisitor* nv) const override
            {
                ++mAsked;
                mHandedNull = mHandedNull || nv == nullptr;
                if (nv != nullptr)
                    mType = nv->getVisitorType();

                return true;
            }

            /// Mutable because the method is const, which is OSG's signature and not a choice here.
            mutable std::size_t mAsked = 0;
            mutable bool mHandedNull = false;

            /// A cull to begin with, so a transform nothing asked fails the same case a wrong answer
            /// does.
            mutable osg::NodeVisitor::VisitorType mType = osg::NodeVisitor::CULL_VISITOR;
        };

        /// Both answers for the same node, so a case can assert they agree without naming either.
        struct Frames
        {
            osg::Matrix mWalked;
            osg::Matrix mOsg;
        };

        Frames framesOf(osg::Node& node)
        {
            const osg::MatrixList worlds = node.getWorldMatrices();
            return Frames{ .mWalked = localToWorldOf(node), .mOsg = worlds.empty() ? osg::Matrix() : worlds.front() };
        }

        /// `localToWorldOf` answers what `getWorldMatrices` answers for its first path.
        ///
        /// **The whole of why the walk may replace the call.** `WrapAroundOperator` asks this of
        /// every particle system on every frame, and the OSG call builds a vector of node paths and
        /// a vector of matrices to answer it — so the walk exists to give the same matrix out of
        /// storage the caller already has. A difference between the two is a different rainstorm in
        /// the renderer nobody is looking at, which is exactly the kind this fork must not cause.
        ///
        /// Every shape the chain above a particle system can take, and two it cannot: a scale, and a
        /// transform that ignores its parents outright.
        TEST(WeatherPrecipitationTest, theWalkAnswersWhatOsgAnswersForTheFirstPath)
        {
            osg::ref_ptr<osg::Node> alone = new osg::Node;
            {
                const Frames frames = framesOf(*alone);
                EXPECT_EQ(frames.mWalked, frames.mOsg) << "a node with no parent at all";
                EXPECT_EQ(frames.mWalked, osg::Matrix()) << "which is the identity";
            }

            // A plain group carries nothing, which is what the rasterizer puts above the systems.
            osg::ref_ptr<osg::Group> group = new osg::Group;
            osg::ref_ptr<osg::Node> underGroup = new osg::Node;
            group->addChild(underGroup);
            {
                const Frames frames = framesOf(*underGroup);
                EXPECT_EQ(frames.mWalked, frames.mOsg) << "a group above";
            }

            // A translation, which is what both hosts put above `Precipitation`.
            osg::ref_ptr<osg::MatrixTransform> moved = new osg::MatrixTransform(osg::Matrix::translate(1.0, 2.0, 3.0));
            osg::ref_ptr<osg::Node> underMoved = new osg::Node;
            moved->addChild(underMoved);
            group->addChild(moved);
            {
                const Frames frames = framesOf(*underMoved);
                EXPECT_EQ(frames.mWalked, frames.mOsg) << "a translation above";
            }

            // A rotation, which is what a storm's own node carries and what turns as it aims.
            osg::ref_ptr<osg::PositionAttitudeTransform> aimed = new osg::PositionAttitudeTransform;
            aimed->setPosition(osg::Vec3f(4.0f, 5.0f, 6.0f));
            aimed->setAttitude(osg::Quat(0.7, osg::Vec3f(0.0f, 0.0f, 1.0f)));
            osg::ref_ptr<osg::Node> underAimed = new osg::Node;
            aimed->addChild(underAimed);
            moved->addChild(aimed);
            {
                const Frames frames = framesOf(*underAimed);
                EXPECT_EQ(frames.mWalked, frames.mOsg) << "a rotation under a translation";
            }

            // A scale as well, so the walk is not only tested where the matrix is a rigid motion.
            osg::ref_ptr<osg::MatrixTransform> widened = new osg::MatrixTransform(osg::Matrix::scale(2.0, 3.0, 4.0));
            osg::ref_ptr<osg::Node> underWidened = new osg::Node;
            widened->addChild(underWidened);
            aimed->addChild(widened);
            {
                const Frames frames = framesOf(*underWidened);
                EXPECT_EQ(frames.mWalked, frames.mOsg) << "a scale under a rotation under a translation";

                // So that the agreements above are two answers about a real frame rather than two
                // identities.
                EXPECT_NE(frames.mWalked, osg::Matrix()) << "the chain carried nothing at all";
            }

            // **A second parent, because the walk follows the first at every level and the OSG call
            // collects every path and is read at its front.** The two orders have to agree or the
            // walk would answer about a chain nothing draws through.
            osg::ref_ptr<osg::MatrixTransform> second
                = new osg::MatrixTransform(osg::Matrix::translate(-9.0, 0.0, 0.0));
            second->addChild(underWidened);
            {
                const Frames frames = framesOf(*underWidened);
                EXPECT_EQ(frames.mWalked, frames.mOsg) << "two parents, and the first is the one walked";
            }

            // **A transform that says it is absolute stops the accumulation at itself**, which
            // `osg::Transform::computeLocalToWorldMatrix` does rather than the walk — so this says
            // the walk leaves that to OSG rather than deciding it.
            osg::ref_ptr<osg::MatrixTransform> absolute
                = new osg::MatrixTransform(osg::Matrix::translate(7.0, 8.0, 9.0));
            absolute->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
            osg::ref_ptr<osg::Node> underAbsolute = new osg::Node;
            absolute->addChild(underAbsolute);
            aimed->addChild(absolute);
            {
                const Frames frames = framesOf(*underAbsolute);
                EXPECT_EQ(frames.mWalked, frames.mOsg) << "an absolute reference frame above";
                EXPECT_EQ(frames.mWalked, osg::Matrix::translate(7.0, 8.0, 9.0)) << "which keeps nothing above it";
            }
        }

        /// Every transform in the chain is handed a visitor, and none is told it is a cull.
        ///
        /// **Both halves are a crash in the sky.** The game hangs its precipitation under
        /// `MWRender::CameraRelativeTransform`: a null visitor there is a read through address
        /// nought, which is where a rainstorm walked by the ray tracer's mirror took the process
        /// down, and a cull visitor is a cast to `osgUtil::CullVisitor` of something that is not
        /// one. Neither is hypothetical — the mirror calls itself a cull visitor for as long as it
        /// steps a particle system, and this walk runs from inside that step.
        TEST(WeatherPrecipitationTest, theWalkHandsEveryTransformAVisitorAndNeverACullOne)
        {
            osg::ref_ptr<RecordsTheVisitor> above = new RecordsTheVisitor;
            osg::ref_ptr<RecordsTheVisitor> below = new RecordsTheVisitor;
            osg::ref_ptr<osg::Node> leaf = new osg::Node;

            above->addChild(below);
            below->addChild(leaf);

            localToWorldOf(*leaf);

            for (const RecordsTheVisitor* transform : { above.get(), below.get() })
            {
                EXPECT_EQ(transform->mAsked, std::size_t{ 1 }) << "asked once, and it was not";
                EXPECT_FALSE(transform->mHandedNull) << "a null visitor reached a transform";
                EXPECT_NE(transform->mType, osg::NodeVisitor::CULL_VISITOR) << "the walk claimed a cull";
            }
        }

        /// Walking the chain reaches the heap not at all, which is the whole reason it exists.
        ///
        /// **Asked of every particle system on every frame the weather draws.** `getWorldMatrices`
        /// answers the same question out of a vector of node paths and a vector of matrices; this
        /// answers it out of one matrix on the stack.
        ///
        /// Warmed up first, because the first of anything legitimately allocates.
        TEST(WeatherPrecipitationTest, walkingTheChainDoesNotTouchTheHeap)
        {
            osg::ref_ptr<osg::MatrixTransform> root = new osg::MatrixTransform(osg::Matrix::translate(1.0, 2.0, 3.0));
            osg::ref_ptr<osg::PositionAttitudeTransform> aimed = new osg::PositionAttitudeTransform;
            aimed->setAttitude(osg::Quat(0.7, osg::Vec3f(0.0f, 0.0f, 1.0f)));
            osg::ref_ptr<osg::Node> leaf = new osg::Node;

            root->addChild(aimed);
            aimed->addChild(leaf);

            const osg::Matrix first = localToWorldOf(*leaf);

            const std::size_t before = Rtx::Testing::getAllocationCount();
            const osg::Matrix again = localToWorldOf(*leaf);
            const std::size_t after = Rtx::Testing::getAllocationCount();

            EXPECT_EQ(after, before) << after - before << " allocations to walk a chain of two";
            EXPECT_EQ(again, first);
        }
    }
}
