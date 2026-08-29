#include "instancerecord.hpp"

#include <span>

#include "shaders/scene.h"

namespace Rtx
{
    namespace
    {
        /// The motion of something that has not moved, written out rather than built from a matrix.
        constexpr Transform3x4 sStillTransform{ { { 1.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f, 0.0f } } };

        /// `transform` with its translation taken `drop` units down the world's up axis.
        ///
        /// The translation is the world position of the placement's origin whatever the rest of the
        /// matrix says, so subtracting from it lowers the instance in the world and not along some
        /// axis of its own.
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

    namespace
    {
        /// The row for `slot` as it stands, still: the motion is the frame's question and is asked
        /// afterwards.
        InstanceRecord recordOf(const SceneDesc& scene, const Index slot)
        {
            const MeshInstance& instance = scene.getInstances()[slot];
            if (!instance.isPlaced())
                return InstanceRecord{};

            // A plain opaque surface where the instance carries no material, which the untextured
            // test scenes place.
            const std::span<const Material> materials = scene.getMaterials();
            const Material::Traversed worn
                = instance.mMaterial == sNoIndex ? Material::Traversed{} : materials[instance.mMaterial].getTraversed();
            const bool water = worn.mKind == MaterialKind::Water;

            // **Here because this is the one funnel.** `WATER_TIE_BREAK` says why the sea is dropped
            // at all; it is dropped here rather than wherever any one sea is made because every
            // water surface in the project reaches the device through this line — the game's
            // mirrored ocean, the harness's own plane, and the sheets the tests place — and a
            // tie-break that some of them missed would be worse than none.
            //
            // On the placement rather than the mesh, so it holds however the surface was authored.
            const osg::Matrixf placement
                = water ? loweredBy(instance.mTransform, Shaders::WATER_TIE_BREAK) : instance.mTransform;

            return InstanceRecord{
                .mTransform = toTransform3x4(placement),
                .mMotion = sStillTransform,
                .mMesh = instance.mMesh,
                .mMask = water              ? Shaders::MASK_WATER
                    : instance.mFirstPerson ? Shaders::MASK_FIRST_PERSON
                                            : Shaders::MASK_SOLID,

                .mCutout = worn.mCutout,
                .mTranslucent = instance.mOpacity < 1.0f || worn.mTranslucent,
                .mPlaced = true,
            };
        }

        /// Gives `record` the motion of a slot that stood somewhere else last frame, and leaves a
        /// slot that stood where it stands — a fade, a placement made this frame — still.
        ///
        /// **The scene says what moved rather than every record being asked.** Comparing each
        /// placement against where it was costs sixteen floats per instance per frame to discover
        /// that a world of statics is still a world of statics; the walk that placed them already
        /// knew, and this is where it is spent instead.
        ///
        /// **The inverse is taken here and never on the device.** A shader that inverted a transform
        /// per hit would do it a million times a frame for an answer that changes once an instance.
        ///
        /// **Built from the placements the scene gave rather than the dropped ones**, and the sea's
        /// drop falls out of the answer rather than being ignored by it: this is
        /// `inverse(current) * previous`, so a translation applied to both cancels — but only while
        /// the placement carries no rotation, which the sea's does not and a rotating one would.
        void moveRecord(const SceneDesc& scene, const Index slot, InstanceRecord& record)
        {
            const MeshInstance& instance = scene.getInstances()[slot];
            if (!instance.isPlaced())
                return;

            // **A slot can be on the list without having moved**: one just placed, or one that
            // faded, has a previous transform that is where it already is. It keeps the identity
            // outright rather than an inverse times itself — `inverse(T) * T` is the identity in
            // arithmetic and not in floats, and a few ulps of a six-figure world coordinate is a
            // fraction of a pixel of drift under a static surface.
            const osg::Matrixf& previous = scene.getPrevious()[slot];
            if (previous == instance.mTransform)
                return;

            record.mMotion = toTransform3x4(osg::Matrixf::inverse(instance.mTransform) * previous);
        }
    }

    void makeInstanceRecords(const SceneDesc& scene, std::vector<InstanceRecord>& records)
    {
        // **Resized and not cleared.** `clear` plus `resize` writes the whole array twice — once
        // with zeroes and once with the records — and at a hundred bytes a slot over fifty thousand
        // slots that is five megabytes of pointless stores. The buffer is the caller's and keeps
        // its size between scenes; only a scene that grew or shrank pays anything here.
        const std::span<const MeshInstance> instances = scene.getInstances();
        records.resize(instances.size());

        for (std::size_t slot = 0; slot < instances.size(); ++slot)
            records[slot] = recordOf(scene, static_cast<Index>(slot));

        for (const Index slot : scene.getMoved())
            moveRecord(scene, slot, records[slot]);
    }

    void updateInstanceRecords(const SceneDesc& scene, std::vector<InstanceRecord>& records)
    {
        records.resize(scene.getInstances().size());

        // The settled first: a slot that moved again since it settled is in both lists, and the
        // pass that gives it its motion has to be the one that wins.
        for (const Index slot : scene.getSettled())
            records[slot] = recordOf(scene, slot);

        for (const Index slot : scene.getMoved())
        {
            records[slot] = recordOf(scene, slot);
            moveRecord(scene, slot, records[slot]);
        }
    }
}
