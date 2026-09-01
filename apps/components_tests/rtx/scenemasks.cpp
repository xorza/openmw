#include <array>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Vec3f>
#include <osg/Vec4f>

#include <components/rtx/scenedesc.hpp>
#include <components/rtx/scenemasks.hpp>
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

        std::vector<Index> everyMesh(const SceneDesc& scene)
        {
            std::vector<Index> meshes(scene.getMeshes().size());
            for (Index at = 0; at < meshes.size(); ++at)
                meshes[at] = at;

            return meshes;
        }

        /// Which mesh a micromap can be built for, over every reason there is not to build one.
        ///
        /// **The filter both sides read, so a drift between them is what this catches.**
        /// `SceneMasks` opens the masks a build will read and `SceneAcceleration::buildMicromaps`
        /// classifies against them; the two agreeing is what makes a mask opened one that is used,
        /// and a mesh classified one whose mask was opened.
        TEST(RtxSceneMasksTest, onlyAMeshUnderOneOpaqueCutoutIsACandidate)
        {
            SceneDesc scene;
            const Index quad = scene.addMesh(sQuad, {}, {}, sQuadIndices);
            const Index leaf = scene.addMesh(sQuad, {}, {}, sQuadIndices);
            const Index pane = scene.addMesh(sQuad, {}, {}, sQuadIndices);
            const Index plain = scene.addMesh(sQuad, {}, {}, sQuadIndices);
            const Index shared = scene.addMesh(sQuad, {}, {}, sQuadIndices);
            const Index unplaced = scene.addMesh(sQuad, {}, {}, sQuadIndices);

            const Index bark = scene.addTexture(VFS::Path::NormalizedView("textures/bark.dds"));
            const Index canopy = scene.addTexture(VFS::Path::NormalizedView("textures/canopy.dds"));

            const Index cutout = scene.addMaterial(Material{
                .mDiffuse = canopy,
                .mAlphaRef = 0.5f,
                .mAlphaMode = AlphaMode::Cutout,
            });
            const Index second = scene.addMaterial(Material{
                .mDiffuse = bark,
                .mAlphaRef = 0.5f,
                .mAlphaMode = AlphaMode::Cutout,
            });

            // Both, which is what `isTranslucent` is: a pane blends *and* its own colour is not
            // solid, so `getAlphaCutoff` gives it a cutoff and the micromap must still refuse it.
            const Index glass = scene.addMaterial(Material{
                .mDiffuse = canopy,
                .mDiffuseColour = osg::Vec4f(1.0f, 1.0f, 1.0f, 0.5f),
                .mAlphaRef = 0.5f,
                .mAlphaMode = AlphaMode::Blend,
            });
            const Index opaque = scene.addMaterial(Material{ .mDiffuse = bark });

            scene.addInstance(MeshInstance{ .mMesh = leaf, .mMaterial = cutout });
            scene.addInstance(MeshInstance{ .mMesh = pane, .mMaterial = glass });
            scene.addInstance(MeshInstance{ .mMesh = plain, .mMaterial = opaque });

            // Twice under the same cutout, which is the ordinary case and must stay a candidate.
            scene.addInstance(MeshInstance{ .mMesh = quad, .mMaterial = cutout });
            scene.addInstance(MeshInstance{ .mMesh = quad, .mMaterial = cutout });

            // A micromap belongs to the mesh and a cutout to the material, so a mesh two cutouts
            // disagree about has no one answer and gets none.
            scene.addInstance(MeshInstance{ .mMesh = shared, .mMaterial = cutout });
            scene.addInstance(MeshInstance{ .mMesh = shared, .mMaterial = second });

            std::vector<Index> materialOfMesh;
            std::vector<MicromapCandidate> candidates;
            micromapCandidates(scene, everyMesh(scene), materialOfMesh, candidates);

            std::vector<Index> found;
            for (const MicromapCandidate& candidate : candidates)
            {
                found.push_back(candidate.mMesh);
                EXPECT_EQ(candidate.mMaterial, cutout) << "mesh " << candidate.mMesh;
            }

            EXPECT_EQ(found, (std::vector<Index>{ quad, leaf }))
                << "mesh " << unplaced << " stands nowhere and mesh " << shared << " stands under two materials";

            // The list it is asked about is what it answers about, which is what makes an extend's
            // arrivals and a rebuild's whole table one code path.
            micromapCandidates(scene, std::span(&pane, 1), materialOfMesh, candidates);
            EXPECT_TRUE(candidates.empty());

            micromapCandidates(scene, std::span(&leaf, 1), materialOfMesh, candidates);
            ASSERT_EQ(candidates.size(), std::size_t{ 1 });
            EXPECT_EQ(candidates.front().mMesh, leaf);
        }
    }
}
