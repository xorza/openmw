#include "instancerecord.hpp"

#include <cstddef>
#include <span>

#include <osg/Vec3f>
#include <osg/Vec4f>

#include "material.hpp"
#include "mesh.hpp"
#include "runs.hpp"
#include "shaders/scene.h"
#include "shaders/skinning.h"

namespace Rtx
{
    namespace
    {
        /// The motion of something that has not moved, written out rather than built from a matrix.
        constexpr Transform3x4 sStillTransform{ { { 1.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f, 0.0f } } };

        /// `transform` with its translation taken `drop` units down the world's up axis.
        osg::Matrixf loweredBy(const osg::Matrixf& transform, float drop)
        {
            osg::Matrixf lowered = transform;
            lowered.setTrans(lowered.getTrans() - osg::Vec3f(0.0f, 0.0f, drop));
            return lowered;
        }
    }

    Transform3x4 toTransform3x4(const osg::Matrixf& matrix)
    {
        Transform3x4 result{};
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column)
                result.mRows[row][column] = matrix(column, row);

            result.mRows[row][3] = matrix(3, row);
        }
        return result;
    }

    Shaders::GpuBone toGpuBone(const osg::Matrixf& matrix)
    {
        const Transform3x4 rows = toTransform3x4(matrix);

        Shaders::GpuBone bone;
        for (int row = 0; row < 3; ++row)
            bone.mRows[row]
                = osg::Vec4f(rows.mRows[row][0], rows.mRows[row][1], rows.mRows[row][2], rows.mRows[row][3]);

        return bone;
    }

    namespace
    {
        /// The row for `slot` as it stands, still: the motion is the frame's question and is asked
        /// afterwards.
        InstanceRecord recordOf(const SceneDesc& scene, const Index slot)
        {
            const PlacementRow& row = scene.placements().getRows()[slot];
            const MeshInstance& instance = row.mInstance;
            if (!instance.isPlaced())
                return InstanceRecord{};

            const Material::Traversed& worn = row.mWorn;
            const PlacedTraversal traversed = worn.placedAt(instance.mOpacity);
            const bool water = worn.mKind == MaterialKind::Water;

            // Here because this is the one funnel every water surface reaches the device through;
            // `WATER_TIE_BREAK` says why the sea is dropped at all. On the placement rather than the
            // mesh, so it holds however the surface was authored.
            const osg::Matrixf placement
                = water ? loweredBy(instance.mTransform, Shaders::WATER_TIE_BREAK) : instance.mTransform;

            return InstanceRecord{
                .mTransform = toTransform3x4(placement),
                .mMotion = sStillTransform,
                .mMesh = instance.mMesh,
                .mKind = worn.mKind,
                // The medium bit rides with whichever of the three this is, so that every ray
                // meets it exactly as it did and one more ray can ask for it alone. Everything that
                // reads a row's mask tests the bit it wants rather than the whole word, which is
                // what makes a second bit free to ride here.
                //
                // **An additive surface carries its own bit and no other.** Nothing that shades,
                // shadows or bounces casts with it, so such a surface is met by the one query that
                // gathers what adds and by nothing else — which is what the rasterizer's
                // shadow-casting masks say of a magic effect too.
                .mMask = traversed.mAdditive ? Shaders::MASK_ADDITIVE
                                             : (water ? Shaders::MASK_WATER : classBit(instance.mClass))
                        | (traversed.mMedium ? Shaders::MASK_MEDIUM : 0u),

                .mCutout = traversed.mCutout,
                .mTranslucent = traversed.mTranslucent,
                .mAdditive = traversed.mAdditive,
                .mTwoSided = worn.mTwoSided || scene.meshes().getRows()[instance.mMesh].mShape.mFolded,
                .mPlaced = true,
            };
        }

        /// Gives `record` the motion of a slot that stood somewhere else last frame, and leaves a
        /// slot that stood where it stands still. The inverse is taken here and never on the
        /// device, which would do it a million times a frame. Built from the placements the scene
        /// gave rather than the dropped ones: `inverse(current) * previous` cancels a translation
        /// applied to both, while the placement carries no rotation, which the sea's does not.
        void moveRecord(const SceneDesc& scene, const Index slot, InstanceRecord& record)
        {
            const PlacementRow& row = scene.placements().getRows()[slot];
            const MeshInstance& instance = row.mInstance;
            if (!instance.isPlaced())
                return;

            // A slot can be on the list without having moved — just placed, or faded — and keeps
            // the identity outright: `inverse(T) * T` is a few ulps of a six-figure coordinate in
            // floats, which is a fraction of a pixel of drift under a static surface.
            if (row.mPrevious == instance.mTransform)
                return;

            record.mMotion = toTransform3x4(osg::Matrixf::inverse(instance.mTransform) * row.mPrevious);
        }
    }

    void makeInstanceRecords(const SceneDesc& scene, std::vector<InstanceRecord>& records)
    {
        // Resized and not cleared. `clear` plus `resize` writes the whole array twice — once
        // with zeroes and once with the records — and at a hundred bytes a slot over fifty thousand
        // slots that is five megabytes of pointless stores. The buffer is the caller's and keeps
        // its size between scenes; only a scene that grew or shrank pays anything here.
        const std::size_t slots = scene.placements().getRows().size();
        records.resize(slots);

        for (std::size_t slot = 0; slot < slots; ++slot)
            records[slot] = recordOf(scene, static_cast<Index>(slot));

        for (const Index slot : scene.placements().getMoved())
            moveRecord(scene, slot, records[slot]);
    }

    void updateInstanceRecords(
        const SceneDesc& scene, std::vector<InstanceRecord>& records, std::vector<Index>& changed)
    {
        records.resize(scene.placements().getRows().size());
        changed.clear();
        changed.reserve(scene.placements().getSettled().size() + scene.placements().getMoved().size());

        // The settled first: a slot that moved again since it settled is in both lists, and the
        // pass that gives it its motion has to be the one that wins.
        for (const Index slot : scene.placements().getSettled())
        {
            records[slot] = recordOf(scene, slot);
            changed.push_back(slot);
        }

        for (const Index slot : scene.placements().getMoved())
        {
            records[slot] = recordOf(scene, slot);
            moveRecord(scene, slot, records[slot]);
            changed.push_back(slot);
        }
    }
}
