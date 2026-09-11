#include <array>

#include <gtest/gtest.h>

#include <osg/Group>
#include <osg/MatrixTransform>
#include <osg/Sequence>
#include <osg/ref_ptr>
#include <osgParticle/ModularEmitter>
#include <osgParticle/ParticleSystem>
#include <osgParticle/ParticleSystemUpdater>

#include <components/rtx/nodekind.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/morphgeometry.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/skeleton.hpp>

namespace Rtx
{
    namespace
    {
        struct Named
        {
            osg::ref_ptr<osg::Object> mObject;
            NodeKind mKind;
            const char* mName;
        };

        /// One object of every kind, a subclass for one of them, and two plain classes.
        std::array<Named, 10> everyKind()
        {
            return {
                Named{ new osg::Group, NodeKind::Other, "Group" },
                Named{ new osg::MatrixTransform, NodeKind::Other, "MatrixTransform" },
                Named{ new SceneUtil::Skeleton, NodeKind::Skeleton, "Skeleton" },
                Named{ new SceneUtil::LightSource, NodeKind::LightSource, "LightSource" },
                Named{ new osgParticle::ParticleSystem, NodeKind::ParticleSystem, "ParticleSystem" },
                Named{ new osgParticle::ModularEmitter, NodeKind::ParticleProcessor, "ModularEmitter" },
                Named{ new osgParticle::ParticleSystemUpdater, NodeKind::ParticleUpdater, "ParticleSystemUpdater" },
                Named{ new osg::Sequence, NodeKind::Sequence, "Sequence" },
                Named{ new SceneUtil::RigGeometry, NodeKind::RigGeometry, "RigGeometry" },
                Named{ new SceneUtil::MorphGeometry, NodeKind::MorphGeometry, "MorphGeometry" },
            };
        }

        /// Every class answers its kind, a subclass answers its base's, and the second ask agrees
        /// with the first — which is the one place a memo could hand out the wrong entry.
        TEST(RtxNodeKindTest, everyKindIsAnsweredAndTheSecondAskAgrees)
        {
            const NodeKinds kinds;

            for (const Named& named : everyKind())
            {
                EXPECT_EQ(kinds.of(*named.mObject), named.mKind) << "at " << named.mName;
                EXPECT_EQ(kinds.of(*named.mObject), named.mKind) << "the second ask at " << named.mName;
            }

            EXPECT_EQ(kinds.getOverflow(), 0u) << "ten classes fit the table";
        }

        /// A table holding every class still tells them apart when asked round and round: a memo
        /// answering from the first entry it kept, or walking one slot too far, passes the test
        /// above and fails this one.
        TEST(RtxNodeKindTest, aTableHoldingEveryClassStillTellsThemApart)
        {
            const NodeKinds kinds;
            const std::array<Named, 10> named = everyKind();

            for (int round = 0; round < 3; ++round)
                for (const Named& one : named)
                    EXPECT_EQ(kinds.of(*one.mObject), one.mKind) << "round " << round << " at " << one.mName;
        }

        /// `as` hands back the object where the kind matches and nothing where it does not.
        TEST(RtxNodeKindTest, asAnswersOnlyTheKindAsked)
        {
            const NodeKinds kinds;
            osg::ref_ptr<osg::Node> skeleton = new SceneUtil::Skeleton;
            osg::ref_ptr<osg::Node> group = new osg::Group;

            EXPECT_EQ(as<SceneUtil::Skeleton>(kinds.of(*skeleton), NodeKind::Skeleton, *skeleton), skeleton.get());
            EXPECT_EQ(as<SceneUtil::Skeleton>(kinds.of(*group), NodeKind::Skeleton, *group), nullptr);
        }
    }
}
