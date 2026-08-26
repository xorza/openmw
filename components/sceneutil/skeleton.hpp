#ifndef OPENMW_COMPONENTS_NIFOSG_SKELETON_H
#define OPENMW_COMPONENTS_NIFOSG_SKELETON_H

#include <osg/Group>

#include <memory>
#include <unordered_map>

namespace SceneUtil
{

    /// @brief Defines a Bone hierarchy, used for updating of skeleton-space bone matrices.
    /// @note To prevent unnecessary updates, only bones that are used for skinning will be added to this hierarchy.
    class Bone
    {
    public:
        Bone();

        osg::Matrixf mMatrixInSkeletonSpace;

        osg::MatrixTransform* mNode;

        std::vector<std::unique_ptr<Bone>> mChildren;

        /// Update the skeleton-space matrix of this bone and all its children.
        void update(const osg::Matrixf* parentMatrixInSkeletonSpace);
    };

    /// @brief Handles the bone matrices for any number of child RigGeometries.
    /// @par Bones should be created as osg::MatrixTransform children of the skeleton.
    /// To be a referenced by a RigGeometry, a bone needs to have a unique name.
    class Skeleton : public osg::Group
    {
    public:
        Skeleton();
        Skeleton(const Skeleton& copy, const osg::CopyOp& copyop);

        META_Node(SceneUtil, Skeleton)

        /// Retrieve a bone by name.
        Bone* getBone(const std::string& name);

        /// Request an update of bone matrices. May be a no-op if already updated in this frame.
        void updateBoneMatrices(unsigned int traversalNumber);

        enum ActiveType
        {
            Inactive = 0,
            SemiActive, /// Like Active, but don't bother with Update (including new bounding box) if we're off-screen
            Active
        };

        /// Set the skinning active flag. Inactive skeletons will not have their child rigs updated.
        /// You should set this flag to false if you know that bones are not currently moving.
        void setActive(ActiveType active);

        bool getActive() const;

        void traverse(osg::NodeVisitor& nv) override;

        /// Says that something which draws this reached it on this frame.
        ///
        /// **What `SemiActive` is actually asking, with the rasterizer taken out of the question.**
        /// A semi-active skeleton stops updating — and so stops animating — once several
        /// traversals have passed with nothing reaching it, on the reasoning that a renderer only
        /// reaches what it is about to draw. A cull is one way of reaching; it is not the only one,
        /// and a renderer that traces has no off screen to be on the wrong side of. So the answer
        /// is asked for rather than inferred, and `traverse` below is one of its callers.
        ///
        /// **The number is the update traversal's**, because that is the only thing that reads it
        /// back. A caller with a sequence of its own — a mirror poses at numbers that are not
        /// frames — has to hand over the frame regardless, or the comparison is between two clocks.
        void markReached(unsigned int updateTraversalNumber) { mLastReachedFrameNumber = updateTraversalNumber; }

        void markDirty();

        void childInserted(unsigned int) override;
        void childRemoved(unsigned int, unsigned int) override;

    private:
        // The root bone is not a "real" bone, it has no corresponding node in the scene graph.
        // As far as the scene graph goes we support multiple root bones.
        std::unique_ptr<Bone> mRootBone;

        typedef std::unordered_map<std::string, std::vector<osg::MatrixTransform*>> BoneCache;
        BoneCache mBoneCache;
        bool mBoneCacheInit;

        bool mNeedToUpdateBoneMatrices;

        ActiveType mActive;

        unsigned int mLastFrameNumber;
        /// When something that draws this last reached it. See `markReached`.
        unsigned int mLastReachedFrameNumber;
    };

}

#endif
