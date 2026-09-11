#include "nodekind.hpp"

#include <osg/Drawable>
#include <osg/Node>
#include <osg/Sequence>

#include <osgParticle/ParticleProcessor>
#include <osgParticle/ParticleSystem>
#include <osgParticle/ParticleSystemUpdater>

#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/morphgeometry.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/skeleton.hpp>

namespace Rtx
{
    namespace
    {
        NodeKind classify(const osg::Object& object)
        {
            if (dynamic_cast<const SceneUtil::Skeleton*>(&object) != nullptr)
                return NodeKind::Skeleton;
            if (dynamic_cast<const SceneUtil::LightSource*>(&object) != nullptr)
                return NodeKind::LightSource;
            if (dynamic_cast<const osgParticle::ParticleSystem*>(&object) != nullptr)
                return NodeKind::ParticleSystem;
            if (dynamic_cast<const osgParticle::ParticleProcessor*>(&object) != nullptr)
                return NodeKind::ParticleProcessor;
            if (dynamic_cast<const osgParticle::ParticleSystemUpdater*>(&object) != nullptr)
                return NodeKind::ParticleUpdater;
            if (dynamic_cast<const osg::Sequence*>(&object) != nullptr)
                return NodeKind::Sequence;
            if (dynamic_cast<const SceneUtil::RigGeometry*>(&object) != nullptr)
                return NodeKind::RigGeometry;
            if (dynamic_cast<const SceneUtil::MorphGeometry*>(&object) != nullptr)
                return NodeKind::MorphGeometry;

            return NodeKind::Other;
        }
    }

    NodeKind NodeKinds::of(const osg::Object& object) const
    {
        const char* const library = object.libraryName();
        const char* const name = object.className();

        for (std::size_t at = 0; at < mHeld; ++at)
            if (mLearned[at].mLibrary == library && mLearned[at].mClass == name)
                return mLearned[at].mKind;

        return learn(object);
    }

    NodeKind NodeKinds::learn(const osg::Object& object) const
    {
        const NodeKind kind = classify(object);

        if (mHeld < mLearned.size())
        {
            mLearned[mHeld] = Learned{ object.libraryName(), object.className(), kind };
            ++mHeld;
        }
        else
            ++mOverflow;

        return kind;
    }
}
