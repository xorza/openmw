#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <osg/BoundingBox>
#include <osg/Math>
#include <osg/Matrixf>
#include <osg/Vec3f>
#include <vulkan/vulkan_core.h>

#include <components/rtx/instancerecord.hpp>
#include <components/rtx/material.hpp>
#include <components/rtx/mesh.hpp>
#include <components/rtx/placementtable.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/surface.hpp>
#include <components/rtxvulkan/sceneacceleration.hpp>
#include <components/vfs/pathutil.hpp>

#include "support/geometry.hpp"

namespace Rtx
{
    namespace
    {
        /// The rows built from nothing, which is the answer the rows kept across frames must match.
        std::vector<InstanceRecord> whole(const SceneDesc& scene)
        {
            std::vector<InstanceRecord> records;
            makeInstanceRecords(scene, records);
            return records;
        }

        void expectSame(const std::vector<InstanceRecord>& kept, const SceneDesc& scene, const char* when)
        {
            const std::vector<InstanceRecord> fresh = whole(scene);
            ASSERT_EQ(kept.size(), fresh.size()) << when;
            for (std::size_t slot = 0; slot < kept.size(); ++slot)
                EXPECT_TRUE(kept[slot] == fresh[slot]) << when << ": slot " << slot;

            // **The counts that gate the trace are what the records tell traversal**, both being
            // `PlacedTraversal`'s: a cutout counted that no record stops for is a walk the trace
            // pays for nothing, and one a record stops for that is not counted is a hole never
            // asked.
            InstanceCounts told;
            for (const InstanceRecord& record : kept)
            {
                if (!record.mPlaced)
                    continue;
                ++told.mPlaced;
                told.mCutout += record.mCutout ? 1u : 0u;
                told.mMedium += (record.mMask & Shaders::MASK_MEDIUM) != 0 ? 1u : 0u;
                told.mAdditive += record.mAdditive ? 1u : 0u;
            }
            const InstanceCounts& counted = scene.placements().getCounts();
            EXPECT_EQ(counted.mPlaced, told.mPlaced) << when;
            EXPECT_EQ(counted.mCutout, told.mCutout) << when;
            EXPECT_EQ(counted.mMedium, told.mMedium) << when;
            EXPECT_EQ(counted.mAdditive, told.mAdditive) << when;

            // **And the present set is exactly the rows a walk looks for, each once**, whatever
            // counted a row out and back in on the way — a fade does both. Each sphere carries the
            // record's kinds and holds every corner of its box as placed.
            std::vector<Index> present;
            for (std::size_t slot = 0; slot < kept.size(); ++slot)
                if (kept[slot].mPlaced && (kept[slot].mAdditive || (kept[slot].mMask & Shaders::MASK_MEDIUM) != 0))
                    present.push_back(static_cast<Index>(slot));

            std::vector<Index> listed(scene.placements().getPresent().begin(), scene.placements().getPresent().end());
            std::sort(listed.begin(), listed.end());
            EXPECT_EQ(listed, present) << when;

            std::vector<Shaders::GpuPresence> spheres;
            scene.placements().describePresences(scene.meshes().getRows(), spheres);
            ASSERT_EQ(spheres.size(), scene.placements().getPresent().size()) << when;
            for (std::size_t at = 0; at < spheres.size(); ++at)
            {
                const Index slot = scene.placements().getPresent()[at];
                const InstanceRecord& record = kept[slot];
                const std::uint32_t kinds = (record.mAdditive ? Shaders::PRESENCE_ADDITIVE : 0u)
                    | ((record.mMask & Shaders::MASK_MEDIUM) != 0 ? Shaders::PRESENCE_MEDIUM : 0u);
                EXPECT_EQ(spheres[at].mKinds, kinds) << when << ": slot " << slot;

                const MeshInstance& placed = scene.placements().getRows()[slot].mInstance;
                const osg::BoundingBoxf& box = scene.meshes().getRows()[placed.mMesh].mBounds;
                for (unsigned int corner = 0; corner < 8; ++corner)
                    EXPECT_LE((box.corner(corner) * placed.mTransform - spheres[at].mCentre).length(),
                        spheres[at].mRadius * (1.0f + 1e-6f))
                        << when << ": slot " << slot << " corner " << corner;
            }
        }

        /// The rows a frame rewrites are the rows a rebuild would produce, through every kind of
        /// change a placement can go through.
        ///
        /// **The cross-check the incremental path rests on.** `makeInstanceRecords` is the plain
        /// answer — every slot, every frame — and `updateInstanceRecords` is what the frame calls;
        /// the second is only right where it agrees with the first after a placement moved, faded,
        /// was re-classed by its material, was dropped, and was taken over. Each step is checked
        /// on the frame it happens and on the frame after, which is where a motion goes back to
        /// nothing and where a row written only on the frame of the move would keep it for ever.
        TEST(RtxInstanceRecordTest, rowsKeptAcrossFramesAreTheRowsBuiltFromNothing)
        {
            SceneDesc scene;
            const Index mesh = Testing::addQuadMesh(scene);

            const Index cutout = scene.addMaterial(Material{
                .mDiffuse = scene.textures().add(VFS::Path::NormalizedView("textures/leaf.dds")),
                .mAlphaRef = 0.5f,
                .mAlphaMode = AlphaMode::Cutout,
            });
            const Index glass = scene.addMaterial(Material{
                .mOpacity = 0.5f,
                .mAlphaMode = AlphaMode::Blend,
            });
            const Index sea = scene.addMaterial(Material{ .mKind = MaterialKind::Water });
            const Index ground = scene.addMaterial(Material{ .mKind = MaterialKind::Terrain });

            const Index leaf = scene.addInstance(MeshInstance{
                .mTransform = osg::Matrixf::translate(1.0f, 0.0f, 0.0f), .mMesh = mesh, .mMaterial = cutout });
            const Index pane = scene.addInstance(MeshInstance{
                .mTransform = osg::Matrixf::translate(2.0f, 0.0f, 0.0f), .mMesh = mesh, .mMaterial = glass });
            const Index water = scene.addInstance(MeshInstance{ .mMesh = mesh, .mMaterial = sea });
            const Index chunk = scene.addInstance(MeshInstance{
                .mTransform = osg::Matrixf::translate(3.0f, 0.0f, 0.0f), .mMesh = mesh, .mMaterial = ground });

            // And the two a walk along the eye's ray looks for, scaled and turned so a sphere about
            // the box is not the box's own: a cloud's shell and a magic effect's sheet.
            Material shell{ .mOpacity = 0.5f, .mAlphaMode = AlphaMode::Blend };
            shell.mDiffuseNeverSolid = true;
            Material sheet{ .mAlphaMode = AlphaMode::Blend };
            sheet.mBlend = BlendKind::Add;
            const Index cloud = scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::scale(4.0f, 1.0f, 2.0f)
                    * osg::Matrixf::rotate(0.7f, osg::Vec3f(0, 0, 1)) * osg::Matrixf::translate(0.0f, 9.0f, 1.0f),
                .mMesh = mesh,
                .mMaterial = scene.addMaterial(shell) });
            const Index glow = scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::translate(-4.0f, 0.0f, 2.0f),
                .mMesh = mesh,
                .mMaterial = scene.addMaterial(sheet) });

            std::vector<InstanceRecord> kept;
            std::vector<Index> changed;
            makeInstanceRecords(scene, kept);
            expectSame(kept, scene, "built");
            EXPECT_TRUE(kept[leaf].mCutout);
            EXPECT_TRUE(kept[pane].mTranslucent);
            EXPECT_EQ(kept[water].mMask, Shaders::MASK_WATER);
            ASSERT_EQ(scene.placements().getPresent().size(), 2u) << "the cloud and the sheet";

            // **The kind, which the backend turns into a shader-table record offset.** Traversal
            // picks the closest-hit shader from it, so a placement carrying the wrong one is shaded
            // by the wrong program — ground as a plain surface, water as ground — and nothing in the
            // build or the layers says so. The three are asserted apart rather than each against a
            // constant, because what a record offset has to be is distinct.
            EXPECT_EQ(kept[leaf].mKind, MaterialKind::Surface);
            EXPECT_EQ(kept[pane].mKind, MaterialKind::Surface);
            EXPECT_EQ(kept[water].mKind, MaterialKind::Water);
            EXPECT_EQ(kept[chunk].mKind, MaterialKind::Terrain);
            EXPECT_NE(kept[water].mKind, kept[chunk].mKind);
            EXPECT_NE(kept[chunk].mKind, kept[leaf].mKind);

            const Transform3x4 still = toTransform3x4(osg::Matrixf::identity());

            // A move: the motion appears on the frame of the move and goes on the frame after.
            scene.placements().advance();
            scene.placements().move(leaf, osg::Matrixf::translate(1.0f, 0.0f, 5.0f));
            updateInstanceRecords(scene, kept, changed);
            expectSame(kept, scene, "moved");
            EXPECT_FALSE(kept[leaf].mMotion == still) << "a mover carried no motion";
            // The four the build placed, settling for the first time, and then the leaf again for
            // its move: a slot in both lists is a row written twice, which costs one row twice.
            EXPECT_EQ(changed, (std::vector<Index>{ leaf, pane, water, chunk, cloud, glow, leaf }))
                << "the slots written, in order";

            scene.placements().advance();
            updateInstanceRecords(scene, kept, changed);
            expectSame(kept, scene, "settled");
            EXPECT_TRUE(kept[leaf].mMotion == still) << "the frame after a move carried the motion on";
            EXPECT_EQ(changed, (std::vector<Index>{ leaf })) << "a settling slot is a row a backend rewrites";

            // A fade re-classes the row and moves nothing. The cloud's is counted out and back in,
            // and stays in the present set once; the sheet moves, and its sphere with it.
            scene.placements().fade(leaf, 0.5f);
            scene.placements().fade(cloud, 0.25f);
            scene.placements().move(glow, osg::Matrixf::translate(-40.0f, 7.0f, 2.0f));
            updateInstanceRecords(scene, kept, changed);
            expectSame(kept, scene, "faded");
            EXPECT_TRUE(kept[leaf].mTranslucent);
            EXPECT_FALSE(kept[leaf].mCutout)
                << "a faded leaf is asked how much of it there is, not whether it is a hole";
            EXPECT_EQ(scene.placements().getCounts().mCutout, 0u);
            EXPECT_TRUE(kept[leaf].mMotion == still);
            scene.placements().advance();

            // A material crossing opaque re-classes the placement wearing it.
            Material solid = scene.materials().getRows()[glass];
            solid.mOpacity = 1.0f;
            scene.setMaterial(glass, solid);
            updateInstanceRecords(scene, kept, changed);
            expectSame(kept, scene, "re-classed");
            EXPECT_FALSE(kept[pane].mTranslucent) << "a pane gone opaque still stops traversal to ask";
            scene.placements().advance();

            // A drop empties the row; the slot taken over is a new row, and the table grows past it.
            // The sheet goes too, and out of the present set.
            scene.placements().drop(pane, Stander::Walk);
            scene.placements().drop(glow, Stander::Walk);
            updateInstanceRecords(scene, kept, changed);
            expectSame(kept, scene, "dropped");
            EXPECT_FALSE(kept[pane].mPlaced);
            EXPECT_EQ(scene.placements().getPresent().size(), 1u) << "the cloud alone";
            scene.placements().advance();

            ASSERT_EQ(
                scene.addInstance(MeshInstance{
                    .mTransform = osg::Matrixf::translate(0.0f, 3.0f, 0.0f), .mMesh = mesh, .mMaterial = cutout }),
                pane);
            const Index more = scene.addInstance(MeshInstance{ .mMesh = mesh });
            updateInstanceRecords(scene, kept, changed);
            expectSame(kept, scene, "taken over and grown");
            EXPECT_TRUE(kept[pane].mPlaced);
            EXPECT_TRUE(kept[pane].mCutout) << "the slot's new tenant, not its last";
            EXPECT_TRUE(kept[pane].mMotion == still) << "a placement made this frame arrived from nowhere";
            EXPECT_EQ(kept.size(), std::size_t{ more } + 1);
        }

        /// Which faces a row is drawn from.
        ///
        /// **Morrowind states both faces two ways.** The content turns `GL_CULL_FACE` off, which is
        /// `Material::mTwoSided`, or it doubles the shape with a reversed twin — which `ShapeFold`
        /// folds back into one triangle and records as `FoldedShape::mFolded`, whether it doubled
        /// the whole shape or a hem of it. A leaf culled after that fold would be gone from one
        /// side, so either says both faces.
        TEST(RtxInstanceRecordTest, aRowSaysWhichFacesItIsDrawnFrom)
        {
            SceneDesc scene;
            const Index plain = Testing::addQuadMesh(scene);
            const Index doubled
                = scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad, .mIndices = Testing::sQuadIndices },
                    FoldedShape{ .mSheet = true, .mFolded = true });
            const Index hemmed
                = scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad, .mIndices = Testing::sQuadIndices },
                    FoldedShape{ .mFolded = true });

            const Index opaque = scene.addMaterial(Material{});
            const Index bothFaces = scene.addMaterial(Material{ .mTwoSided = true });

            const Index solid = scene.addInstance(MeshInstance{ .mMesh = plain, .mMaterial = opaque });
            const Index stated = scene.addInstance(MeshInstance{ .mMesh = plain, .mMaterial = bothFaces });
            const Index leaf = scene.addInstance(MeshInstance{ .mMesh = doubled, .mMaterial = opaque });
            const Index awning = scene.addInstance(MeshInstance{ .mMesh = hemmed, .mMaterial = opaque });

            const std::vector<InstanceRecord> records = whole(scene);

            EXPECT_FALSE(records[solid].mTwoSided) << "the scene root culls everything the content does not spare";
            EXPECT_TRUE(records[stated].mTwoSided) << "a material the content turned culling off for";
            EXPECT_TRUE(records[leaf].mTwoSided) << "a shape the content doubled and the fold halved";
            EXPECT_TRUE(records[awning].mTwoSided) << "and one the fold took a twin from anywhere at all";
        }

        /// OpenSceneGraph's transform and an instance descriptor's must move a point to the same
        /// place.
        ///
        /// OSG multiplies a row vector on the left and a descriptor a column vector on the right, so
        /// the conversion is a transpose with the translation moved from the last row to the last
        /// column. Getting it wrong mirrors the world about its diagonal, which symmetrical
        /// architecture hides well enough to survive being looked at.
        ///
        /// Asserted on `Transform3x4`, which is where that transposition happens for every backend.
        TEST(RtxTransformTest, theNeutralTransformMovesAPointWhereOpenSceneGraphWould)
        {
            osg::Matrixf matrix = osg::Matrixf::scale(2.0f, 2.0f, 2.0f)
                * osg::Matrixf::rotate(osg::DegreesToRadians(37.0f), osg::Vec3f(0.3f, -0.5f, 0.8f))
                * osg::Matrixf::translate(11.0f, -23.0f, 5.0f);

            const osg::Vec3f point(3.0f, -5.0f, 7.0f);
            const osg::Vec3f expected = point * matrix;

            const Transform3x4 transform = toTransform3x4(matrix);
            for (int row = 0; row < 3; ++row)
            {
                const float actual = transform.mRows[row][0] * point.x() + transform.mRows[row][1] * point.y()
                    + transform.mRows[row][2] * point.z() + transform.mRows[row][3];
                EXPECT_NEAR(actual, expected[row], 1e-3f) << "row " << row;
            }
        }

        /// Vulkan stores the same three rows of four, so its conversion must not reorder anything.
        ///
        /// Cheap, and it is the assertion a second backend copies: whatever `MTLPackedFloat4x3` or
        /// anything else stores, it has to come back to these twelve numbers in this order.
        TEST(RtxTransformTest, theVulkanTransformRestatesTheNeutralRowsUnchanged)
        {
            const Transform3x4 transform{ { { 1.0f, 2.0f, 3.0f, 4.0f }, { 5.0f, 6.0f, 7.0f, 8.0f },
                { 9.0f, 10.0f, 11.0f, 12.0f } } };

            const VkTransformMatrixKHR converted = toVulkanTransform(transform);
            for (int row = 0; row < 3; ++row)
                for (int column = 0; column < 4; ++column)
                    EXPECT_EQ(converted.matrix[row][column], transform.mRows[row][column])
                        << "row " << row << " column " << column;
        }
    }
}
