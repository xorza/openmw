#ifndef OPENMW_COMPONENTS_SCENEUTIL_STABLEIDENTITY_H
#define OPENMW_COMPONENTS_SCENEUTIL_STABLEIDENTITY_H

#include <cstdint>

#include <osg/CopyOp>
#include <osg/Node>
#include <osg/Object>
#include <osg/UserDataContainer>

namespace SceneUtil
{
    /// A number the engine stamps on a node it stands something at, so that whatever mirrors the
    /// graph can tell one thing from the next by what it is and not by where its node happens to
    /// sit in memory. The game hands one out per reference root, per cell root and for the player;
    /// everything under such a node is told apart by its place in the subtree.
    ///
    /// Kept in the node's user data slot (`osg::Object::setUserData`) rather than among its user
    /// objects, because nothing else in the engine writes that slot on a node and a reader then
    /// finds it in one load rather than a scan.
    class StableIdentity : public osg::Object
    {
    public:
        StableIdentity() = default;

        explicit StableIdentity(const std::uint64_t id)
            : mId(id)
        {
        }

        StableIdentity(const StableIdentity& copy, const osg::CopyOp& copyop = osg::CopyOp::SHALLOW_COPY)
            : osg::Object(copy, copyop)
            , mId(copy.mId)
        {
        }

        META_Object(SceneUtil, StableIdentity)

        std::uint64_t getId() const { return mId; }

        static void stamp(osg::Node& node, const std::uint64_t id) { node.setUserData(new StableIdentity(id)); }

        /// The identity `node` carries, or null where the engine stamped none.
        static const StableIdentity* find(const osg::Node& node)
        {
            const osg::UserDataContainer* held = node.getUserDataContainer();
            if (held == nullptr)
                return nullptr;

            return dynamic_cast<const StableIdentity*>(held->getUserData());
        }

    private:
        std::uint64_t mId = 0;
    };
}

#endif
