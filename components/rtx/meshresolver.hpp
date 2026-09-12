#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <osg/BoundingBox>
#include <osg/Vec2f>
#include <osg/Vec3f>

// For `RigGeometry::InfluenceData`, which is what a rig is keyed on: a nested type cannot be
// forward-declared. It brings `osg::Vec3Array`, which a morph is keyed on, with it.
#include <components/sceneutil/riggeometry.hpp>

#include "index.hpp"
#include "meshreader.hpp"
#include "mirroridentity.hpp"
#include "mirrorpass.hpp"
#include "scenedesc.hpp"
#include "shaders/skinning.h"

namespace osg
{
    class Drawable;
    class Geometry;
}

namespace SceneUtil
{
    class MorphGeometry;
}

namespace Rtx
{
    /// Turns the drawables a walk met into the scene's meshes, and poses the ones that deform.
    ///
    /// **Keyed on the drawable and not on the geometry.** A crate met again is the crate already
    /// uploaded; a body met again is the same mesh posed again, and a pose is bone rows and never
    /// vertices. That identity is what makes an incremental mirror possible instead of a rebuild
    /// per frame, and it is why this holds state at all.
    ///
    /// **The rigs and the morphs are here because a mesh is what names one.** A skin is one
    /// `InfluenceData` however many drawables share it, and a face's targets are one base array
    /// however many heads carry them — so they are resolved, stamped and swept exactly where the
    /// meshes on them are.
    class MeshResolver
    {
    public:
        /// @param pass the walk in progress: its sweep stamp and its counts, read at every call.
        ///        Borrowed, so that the mirror and everything resolving into it cannot come to hold
        ///        two answers.
        MeshResolver(SceneDesc& scene, const MirrorPass& pass)
            : mScene(scene)
            , mPass(pass)
        {
        }

        /// The mesh index for one drawable, adding it or posing it as its kind requires.
        ///
        /// @param material what the drawable wears, resolved first, which a mesh records as it
        ///        arrives — `MeshRange::mMaterial`.
        Index resolve(const osg::Drawable& drawable, const DrawableRead& read, Index material);

        /// The mesh index for a drawable somebody else has already read, adding it where the mirror
        /// does not hold it and stamping it where it does.
        ///
        /// **The insertion half of `resolve`, for a reading made off the frame.** A ring preparing
        /// cells ahead of the eye reads and folds on a thread of its own; what the frame owes is the
        /// copy into the scene and the identity the walk will find the mesh under — which is the
        /// drawable, so that a clone of the same template met by the walk resolves to this mesh
        /// rather than to a copy of it.
        ///
        /// Standing only: a reading carries no rig and no morph, and a drawable the mirror holds as
        /// deforming is not one this may be asked about.
        ///
        /// **One hold is taken on the entry**, which keeps it and the mesh through every sweep until
        /// `release` gives it back — `Known::mHolds` says why a count and not a stamp per walk.
        Index adopt(const osg::Drawable& drawable, const MeshReading& reading, Index material);

        /// Gives one `adopt` back. The mirror must hold `drawable`, which it does for as long as
        /// anything holds it.
        void release(const osg::Drawable& drawable);

        /// Whether every mesh the map holds was met this epoch — see `Kept::whole`. What the
        /// mirror asks before it sweeps, because the survivor list this fills is read beside the
        /// material resolver's.
        bool whole() const { return mMeshes.whole(); }

        /// Drops every mesh neither this epoch nor a hold keeps, and collects the survivors into
        /// `live`.
        void retire(std::vector<Index>& live);

        /// Drops the rigs and the morph targets no mesh named this epoch.
        ///
        /// **Asked whatever the meshes did**, and each map skips its own walk where the epoch
        /// reached all of it. A deformer goes stale only where a mesh on it died, so this is nearly
        /// always the two comparisons and nothing else.
        void retireDeformers();

        /// Reserves the identity maps once, so no frame rehashes them. `SceneExtractor` states the
        /// budgets.
        void reserve(std::size_t meshes, std::size_t deformers)
        {
            mMeshes.reserve(meshes);
            mRigs.reserve(deformers);
            mMorphs.reserve(deformers);
        }

    private:
        /// The scene's rig for a skin, added the first time the skin is met. Shared by every copy of
        /// the drawable, because the skin is.
        Index resolveRig(const SceneUtil::RigGeometry& rig);

        /// The same for a morph's targets, keyed on the base target every copy shares.
        Index resolveMorph(const SceneUtil::MorphGeometry& morph);

        /// Hands the scene this frame's bone rows for `mesh`: `RigGeometry::cull`'s own composition
        /// of each bone's inverse bind, its skeleton-space matrix and the skin transform, from the
        /// matrices the update traversal left.
        void poseRig(Index mesh, const SceneUtil::RigGeometry& rig);

        /// The same with the morph's weights, which its controller wrote under the update traversal.
        void poseMorph(Index mesh, const SceneUtil::MorphGeometry& morph);

        /// What poses one drawable, as the mirror already holds it.
        ///
        /// **The entry and not only the index**, because the stamp wants the one the lookup found:
        /// a second `find` per posed part per frame is a pointer hash and a bucket walk for an
        /// answer already in hand, and a crowded cell poses hundreds. Which of the two entries is
        /// set follows from `DrawableRead::mDeform`, and neither is looked at while `mIndex` is
        /// `sNoIndex`.
        struct Held
        {
            Index mIndex = sNoIndex;

            Identity<const SceneUtil::RigGeometry::InfluenceData>::Entry mRig;
            Identity<const osg::Vec3Array>::Entry mMorph;
        };

        /// The deformer this drawable stands on, where the mirror holds one — and `sNoIndex`
        /// where it does not, which is what a slot that stands holds too, so the fit test compares
        /// the two without a case of its own.
        ///
        /// **Stamps nothing.** Whether the slot still fits is decided after this, and a stamp in
        /// front of that decision would keep a deformer the sweep is about to be told to drop.
        Held holdDeformer(const DrawableRead& read);

        /// The same for a drawable the mirror is meeting afresh: the deformer added and stamped,
        /// or `sNoIndex` where the drawable stands.
        ///
        /// Throws where it does not pose exactly `vertices`. **Named rather than asserted**,
        /// because a vertex count comes out of a content file and a mesh posed by a deformer of
        /// another length is a kernel writing past the run it was handed.
        Index addDeformer(const DrawableRead& read, std::size_t vertices);

        /// Says the walk met what `holdDeformer` found, for a slot the fit test has kept.
        ///
        /// **Stamped with the mesh, which is what keeps the sweep's two answers one answer**: a
        /// deformer the sweep did not see go is one it keeps for as long as a mesh stands on it.
        ///
        /// The arrival path needs none of this: `resolveRig` and `resolveMorph` stamp through
        /// `reach` as they go.
        void stampDeformer(const DrawableRead& read, const Held& held);

        /// Poses `mesh` where the drawable deforms, and counts it. Nothing where it stands.
        void pose(Index mesh, const DrawableRead& read, ExtractionStats& stats);

        SceneDesc& mScene;
        const MirrorPass& mPass;

        // Keyed on pointer identity, which OpenMW's resource cache and its optimizer's
        // SHARE_DUPLICATE_STATE pass together make meaningful: the same model loaded twice is the
        // same object, and equivalent state sets are collapsed into one.
        //
        // **Owning, which is what makes that identity sound.** What these hold outlives the graph
        // by one sweep, and a sweep is what lets go.
        Identity<const osg::Drawable> mMeshes{ mPass };

        /// What the scene knows each skin and each set of morph targets as. Swept with the meshes: a
        /// rig no mesh named this epoch is a rig the scene has let go of.
        Identity<const SceneUtil::RigGeometry::InfluenceData> mRigs{ mPass };
        Identity<const osg::Vec3Array> mMorphs{ mPass };

        MeshReader mReader;

        // Refilled per rig and per morph, which a crowd is hundreds of.
        std::vector<std::uint32_t> mRunScratch;
        std::vector<Shaders::GpuInfluence> mInfluenceScratch;
        std::vector<osg::Vec3f> mOffsetScratch;
        std::vector<Shaders::GpuBone> mBoneScratch;
        std::vector<float> mWeightScratch;
    };
}
