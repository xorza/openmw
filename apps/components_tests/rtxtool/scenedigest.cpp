#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Matrixf>
#include <osg/Vec2f>
#include <osg/Vec3f>

#include <components/rtx/scenedesc.hpp>
#include <components/vfs/pathutil.hpp>

#include <apps/rtxtool/scenedigest.hpp>

namespace RtxTool
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
                positions, normals, texCoords, indices, {}, Rtx::Deform::None, Rtx::sNoIndex, material);
        }

        /// A scene of the two boxes under one placement, added in the order given, and one lamp.
        std::string digestOfBoxes(
            const bool mirroredFirst, const float lift, const float aside, const bool shuffled = false)
        {
            Rtx::SceneDesc scene;
            Rtx::Material material;
            material.mDiffuse = scene.addTexture(VFS::Path::NormalizedView("textures/box.dds"));
            const Rtx::Index wearing = scene.addMaterial(material);

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

            return digestScene(scene);
        }

        /// **Two siblings swapped is one scene, a shape stored in another order is one scene, and
        /// a box moved or reshaped is another.** The first two are what the host's optimizer does
        /// between one process and the next; the digest has to be blind to them and to nothing else.
        TEST(RtxSceneDigestTest, storageOrderIsNotAChangeAndAMovedBoxIs)
        {
            const std::string one = digestOfBoxes(false, 0.0f, 0.0f);
            EXPECT_EQ(one, digestOfBoxes(true, 0.0f, 0.0f)) << "siblings swapped";
            EXPECT_EQ(one, digestOfBoxes(false, 0.0f, 0.0f, true)) << "vertices stored in another order";
            EXPECT_EQ(one.size(), 32u);

            EXPECT_NE(one, digestOfBoxes(false, 1.0f, 0.0f)) << "a vertex moved is a change";
            EXPECT_NE(one, digestOfBoxes(false, 0.0f, 1.0f)) << "a placement moved is a change";
            EXPECT_NE(digestOfBoxes(false, 1.0f, 0.0f), digestOfBoxes(false, 0.0f, 1.0f));
        }
    }
}
