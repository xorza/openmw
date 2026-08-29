#include <array>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Matrixf>
#include <osg/Vec3f>
#include <osg/Vec4f>

#include <components/rtx/instancerecord.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/vfs/pathutil.hpp>

namespace Rtx
{
    namespace
    {
        const std::array<osg::Vec3f, 4> sQuad{
            osg::Vec3f(0.0f, 0.0f, 0.0f),
            osg::Vec3f(1.0f, 0.0f, 0.0f),
            osg::Vec3f(1.0f, 1.0f, 0.0f),
            osg::Vec3f(0.0f, 1.0f, 0.0f),
        };
        constexpr std::array<std::uint32_t, 6> sQuadIndices{ 0, 1, 2, 0, 2, 3 };

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
            const Index mesh = scene.addMesh(sQuad, {}, {}, sQuadIndices);

            const Index cutout = scene.addMaterial(Material{
                .mDiffuse = scene.addTexture(VFS::Path::NormalizedView("textures/leaf.dds")),
                .mAlphaRef = 0.5f,
                .mAlphaMode = AlphaMode::Cutout,
            });
            const Index glass = scene.addMaterial(Material{
                .mDiffuseColour = osg::Vec4f(1.0f, 1.0f, 1.0f, 0.5f),
                .mAlphaMode = AlphaMode::Blend,
            });
            const Index sea = scene.addMaterial(Material{ .mKind = MaterialKind::Water });

            const Index leaf = scene.addInstance(MeshInstance{
                .mTransform = osg::Matrixf::translate(1.0f, 0.0f, 0.0f), .mMesh = mesh, .mMaterial = cutout });
            const Index pane = scene.addInstance(MeshInstance{
                .mTransform = osg::Matrixf::translate(2.0f, 0.0f, 0.0f), .mMesh = mesh, .mMaterial = glass });
            const Index water = scene.addInstance(MeshInstance{ .mMesh = mesh, .mMaterial = sea });

            std::vector<InstanceRecord> kept;
            std::vector<Index> changed;
            makeInstanceRecords(scene, kept);
            expectSame(kept, scene, "built");
            EXPECT_TRUE(kept[leaf].mCutout);
            EXPECT_TRUE(kept[pane].mTranslucent);
            EXPECT_EQ(kept[water].mMask, Shaders::MASK_WATER);

            const Transform3x4 still = toTransform3x4(osg::Matrixf::identity());

            // A move: the motion appears on the frame of the move and goes on the frame after.
            scene.advancePlacement();
            scene.moveInstance(leaf, osg::Matrixf::translate(1.0f, 0.0f, 5.0f));
            updateInstanceRecords(scene, kept, changed);
            expectSame(kept, scene, "moved");
            EXPECT_FALSE(kept[leaf].mMotion == still) << "a mover carried no motion";
            // The three the build placed, settling for the first time, and then the leaf again for
            // its move: a slot in both lists is a row written twice, which costs one row twice.
            EXPECT_EQ(changed, (std::vector<Index>{ leaf, pane, water, leaf })) << "the slots written, in order";

            scene.advancePlacement();
            updateInstanceRecords(scene, kept, changed);
            expectSame(kept, scene, "settled");
            EXPECT_TRUE(kept[leaf].mMotion == still) << "the frame after a move carried the motion on";
            EXPECT_EQ(changed, (std::vector<Index>{ leaf })) << "a settling slot is a row a backend rewrites";

            // A fade re-classes the row and moves nothing.
            scene.fadeInstance(leaf, 0.5f);
            updateInstanceRecords(scene, kept, changed);
            expectSame(kept, scene, "faded");
            EXPECT_TRUE(kept[leaf].mTranslucent);
            EXPECT_TRUE(kept[leaf].mMotion == still);
            scene.advancePlacement();

            // A material crossing opaque re-classes the placement wearing it.
            Material solid = scene.getMaterials()[glass];
            solid.mDiffuseColour.a() = 1.0f;
            scene.setMaterial(glass, solid);
            updateInstanceRecords(scene, kept, changed);
            expectSame(kept, scene, "re-classed");
            EXPECT_FALSE(kept[pane].mTranslucent) << "a pane gone opaque still stops traversal to ask";
            scene.advancePlacement();

            // A drop empties the row; the slot taken over is a new row, and the table grows past it.
            scene.dropInstance(pane);
            updateInstanceRecords(scene, kept, changed);
            expectSame(kept, scene, "dropped");
            EXPECT_FALSE(kept[pane].mPlaced);
            scene.advancePlacement();

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
    }
}
