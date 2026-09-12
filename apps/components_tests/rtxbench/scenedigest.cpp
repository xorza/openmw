#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Matrixf>
#include <osg/Vec2f>
#include <osg/Vec3f>

#include <components/rtx/scenedesc.hpp>
#include <components/rtxbench/framehashes.hpp>
#include <components/rtxbench/scenedigest.hpp>
#include <components/vfs/pathutil.hpp>

namespace Rtx
{
    namespace
    {
        /// An eight-cornered box, mirrored in x where `mirrored`, which is the pair of sibling shapes
        /// the host hands over in heap order. `shuffled` stores the same vertices in reverse and
        /// spells the same triangles from another corner, which is what the host's geometry merge
        /// does to a shape.
        Rtx::Index addBox(Rtx::SceneDesc& scene, const bool mirrored, const Rtx::Index material,
            const float lift = 0.0f, const bool shuffled = false)
        {
            std::vector<osg::Vec3f> positions;
            std::vector<osg::Vec3f> normals;
            std::vector<osg::Vec2f> texCoords;
            for (int corner = 0; corner < 8; ++corner)
            {
                const float x = (corner & 1) != 0 ? 143.0f : 124.0f;
                positions.emplace_back(
                    mirrored ? -x : x, (corner & 2) != 0 ? 1.0f : 0.0f, ((corner & 4) != 0 ? 1.0f : 0.0f) + lift);
                normals.emplace_back(0.0f, 0.0f, 1.0f);
                texCoords.emplace_back(static_cast<float>(corner), 0.0f);
            }

            std::vector<std::uint32_t> indices{ 0, 1, 2, 2, 1, 3 };
            if (shuffled)
            {
                std::reverse(positions.begin(), positions.end());
                std::reverse(texCoords.begin(), texCoords.end());
                indices = { 7, 6, 5, 5, 6, 4 };
            }

            return scene.addMesh(
                MeshArrays{
                    .mPositions = positions, .mNormals = normals, .mTexCoords = texCoords, .mIndices = indices },
                {}, Rtx::Deform::None, Rtx::sNoIndex, material);
        }

        /// The two boxes under one placement, added in the order given, and one lamp.
        void fillBoxes(Rtx::SceneDesc& scene, const bool mirroredFirst, const float lift, const float aside,
            const bool shuffled = false)
        {
            Rtx::Material material;
            material.mDiffuse = scene.textures().add(VFS::Path::NormalizedView("textures/box.dds"));
            const Rtx::Index wearing = scene.materials().add(material);

            const osg::Matrixf stood = osg::Matrixf::translate(10.0f, 20.0f, 30.0f);
            for (int which = 0; which < 2; ++which)
            {
                const bool mirrored = (which == 0) == mirroredFirst;
                Rtx::MeshInstance instance;
                instance.mTransform = mirrored ? osg::Matrixf::translate(aside, 0.0f, 0.0f) * stood : stood;
                instance.mMesh = addBox(scene, mirrored, wearing, mirrored ? lift : 0.0f, shuffled);
                instance.mMaterial = wearing;
                scene.addInstance(instance);
            }

            Rtx::Light lamp;
            lamp.mPosition = osg::Vec3f(1.0f, 2.0f, 3.0f);
            lamp.mIntensity = osg::Vec3f(4.0f, 5.0f, 6.0f);
            scene.addLight(lamp);
        }

        std::string digestOfBoxes(
            const bool mirroredFirst, const float lift, const float aside, const bool shuffled = false)
        {
            Rtx::SceneDesc scene;
            fillBoxes(scene, mirroredFirst, lift, aside, shuffled);
            return spellHash(digestScene(scene.getTables()));
        }

        std::string layoutOfBoxes(
            const bool mirroredFirst, const float lift, const float aside, const bool shuffled = false)
        {
            Rtx::SceneDesc scene;
            fillBoxes(scene, mirroredFirst, lift, aside, shuffled);
            return spellHash(digestLayout(digestParts(scene.getTables())));
        }

        /// **Two siblings swapped is one scene, a shape stored in another order is one scene, and
        /// a box moved or reshaped is another.** The first two are what the host's optimizer does
        /// between one process and the next; the digest has to be blind to them and to nothing else.
        TEST(RtxSceneDigestTest, storageOrderIsNotAChangeAndAMovedBoxIs)
        {
            const std::string one = digestOfBoxes(false, 0.0f, 0.0f);
            EXPECT_EQ(one, digestOfBoxes(true, 0.0f, 0.0f)) << "siblings swapped";
            EXPECT_EQ(one, digestOfBoxes(false, 0.0f, 0.0f, true)) << "vertices stored in another order";

            EXPECT_NE(one, digestOfBoxes(false, 1.0f, 0.0f)) << "a vertex moved is a change";
            EXPECT_NE(one, digestOfBoxes(false, 0.0f, 1.0f)) << "a placement moved is a change";
            EXPECT_NE(digestOfBoxes(false, 1.0f, 0.0f), digestOfBoxes(false, 0.0f, 1.0f));
        }

        /// **The layout digest answers the two questions the other one refuses**, which is the whole
        /// of why there are two. A structure is built over the index buffer as written, so a scene
        /// stored two ways is two scenes to a ray tracer even where it is one cell to a reader.
        TEST(RtxSceneDigestTest, layoutSeesStorageOrderAndEverythingTheOtherDoes)
        {
            const std::string one = layoutOfBoxes(false, 0.0f, 0.0f);
            EXPECT_EQ(one, layoutOfBoxes(false, 0.0f, 0.0f)) << "one scene built twice";

            EXPECT_NE(one, layoutOfBoxes(true, 0.0f, 0.0f)) << "siblings swapped";
            EXPECT_NE(one, layoutOfBoxes(false, 0.0f, 0.0f, true)) << "vertices stored in another order";
            EXPECT_NE(one, layoutOfBoxes(false, 1.0f, 0.0f)) << "a vertex moved";
            EXPECT_NE(one, layoutOfBoxes(false, 0.0f, 1.0f)) << "a placement moved";
        }

        /// **Where the pair disagrees is the fault neither could name alone.** A run whose scene
        /// digest holds while its layout digest moves has been handed one cell stored two ways, and
        /// that is what a report has to be able to say.
        TEST(RtxSceneDigestTest, storageOrderIsWhereTheTwoDigestsPartCompany)
        {
            EXPECT_EQ(digestOfBoxes(false, 0.0f, 0.0f), digestOfBoxes(false, 0.0f, 0.0f, true));
            EXPECT_NE(layoutOfBoxes(false, 0.0f, 0.0f), layoutOfBoxes(false, 0.0f, 0.0f, true));
        }

        /// One box under a material `change` has been applied to, digested both ways.
        std::pair<std::string, std::string> digestsOfMaterial(void (*change)(Rtx::Material&))
        {
            Rtx::SceneDesc scene;

            Rtx::Material material;
            material.mDiffuse = scene.textures().add(VFS::Path::NormalizedView("textures/box.dds"));
            change(material);

            Rtx::MeshInstance instance;
            instance.mMaterial = scene.materials().add(material);
            instance.mMesh = addBox(scene, false, instance.mMaterial);
            scene.addInstance(instance);

            return { spellHash(digestScene(scene.getTables())),
                spellHash(digestLayout(digestParts(scene.getTables()))) };
        }

        /// **Every field of a material reaches both digests**, which is what one field list buys.
        ///
        /// The three flags below are the ones a reader would least expect to matter, and each does:
        /// two runs that disagreed about whether a chunk was flattening would otherwise have held
        /// one scene as far as the digest could say.
        TEST(RtxSceneDigestTest, everyMaterialFieldReachesBothDigests)
        {
            const auto [scene, layout] = digestsOfMaterial([](Rtx::Material&) {});

            for (const auto& [what, change] : std::initializer_list<std::pair<const char*, void (*)(Rtx::Material&)>>{
                     { "flatten", [](Rtx::Material& m) { m.mFlatten = true; } },
                     { "animated", [](Rtx::Material& m) { m.mAnimated = true; } },
                     { "never solid", [](Rtx::Material& m) { m.mDiffuseNeverSolid = true; } },
                 })
            {
                const auto [movedScene, movedLayout] = digestsOfMaterial(change);
                EXPECT_NE(scene, movedScene) << what;
                EXPECT_NE(layout, movedLayout) << what;
            }
        }
    }
}
