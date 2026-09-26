#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <osg/BoundingBox>
#include <osg/Matrixf>
#include <osg/Vec2f>
#include <osg/Vec3f>
#include <osg/Vec4f>

#include <apps/rtxtool/instruments/scenedigest.hpp>
#include <components/rtx/deformertable.hpp>
#include <components/rtx/instancerecord.hpp>
#include <components/rtx/lightbuilder.hpp>
#include <components/rtx/material.hpp>
#include <components/rtx/mesh.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/skinning.h>
#include <components/rtx/sprite.hpp>
#include <components/vfs/pathutil.hpp>

namespace RtxTool
{
    namespace
    {
        /// An eight-cornered box, mirrored in x where `mirrored`, which is the pair of sibling shapes
        /// the host hands over in heap order. `shuffled` stores the same vertices in reverse and
        /// spells the same triangles from another corner, which is what the host's geometry merge
        /// does to a shape. `tangents` is none or one per corner, and is not shuffled.
        Rtx::Index addBox(Rtx::SceneDesc& scene, const bool mirrored, const float lift = 0.0f,
            const bool shuffled = false, std::span<const osg::Vec4f> tangents = {})
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

            return scene.addMesh(Rtx::MeshArrays{ .mPositions = positions,
                                     .mNormals = normals,
                                     .mTexCoords = texCoords,
                                     .mTangents = tangents,
                                     .mIndices = indices },
                {});
        }

        /// The two boxes under one placement, added in the order given, and one lamp.
        void fillBoxes(Rtx::SceneDesc& scene, const bool mirroredFirst, const float lift, const float aside,
            const bool shuffled = false)
        {
            Rtx::Material material;
            material.mDiffuse = scene.textures().add(VFS::Path::NormalizedView("textures/box.dds"));
            const Rtx::Index wearing = scene.addMaterial(material);

            const osg::Matrixf stood = osg::Matrixf::translate(10.0f, 20.0f, 30.0f);
            for (int which = 0; which < 2; ++which)
            {
                const bool mirrored = (which == 0) == mirroredFirst;
                Rtx::MeshInstance instance;
                instance.mTransform = mirrored ? osg::Matrixf::translate(aside, 0.0f, 0.0f) * stood : stood;
                instance.mMesh = addBox(scene, mirrored, mirrored ? lift : 0.0f, shuffled);
                instance.mMaterial = wearing;
                scene.addInstance(instance);
            }

            Rtx::Light lamp{};
            lamp.mPosition = osg::Vec3f(1.0f, 2.0f, 3.0f);
            lamp.mIntensity = osg::Vec3f(4.0f, 5.0f, 6.0f);
            scene.addLight(lamp);
        }

        std::string digestOfBoxes(
            const bool mirroredFirst, const float lift, const float aside, const bool shuffled = false)
        {
            Rtx::SceneDesc scene;
            fillBoxes(scene, mirroredFirst, lift, aside, shuffled);
            return spellHash(digestScene(scene));
        }

        std::string layoutOfBoxes(
            const bool mirroredFirst, const float lift, const float aside, const bool shuffled = false)
        {
            Rtx::SceneDesc scene;
            fillBoxes(scene, mirroredFirst, lift, aside, shuffled);
            return spellHash(digestLayout(digestParts(scene)));
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
            instance.mMaterial = scene.addMaterial(material);
            instance.mMesh = addBox(scene, false);
            scene.addInstance(instance);

            return { spellHash(digestScene(scene)), spellHash(digestLayout(digestParts(scene))) };
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
                     { "parallax", [](Rtx::Material& m) { m.mParallax = true; } },
                 })
            {
                const auto [movedScene, movedLayout] = digestsOfMaterial(change);
                EXPECT_NE(scene, movedScene) << what;
                EXPECT_NE(layout, movedLayout) << what;
            }
        }

        /// One box under one placement, its corners carrying `tangents`, digested whole and part by
        /// part.
        std::pair<std::string, ScenePartDigests> digestsOfTangents(std::span<const osg::Vec4f> tangents)
        {
            Rtx::SceneDesc scene;
            Rtx::MeshInstance instance;
            instance.mMesh = addBox(scene, false, 0.0f, false, tangents);
            scene.addInstance(instance);
            return { spellHash(digestScene(scene)), digestParts(scene) };
        }

        /// **A tangent moves the normals column, the meshes column and the scene, and no tangent
        /// moves nothing.** The words are hashed only where a vertex has one and the mesh row's
        /// claim to them only where it is made, so a box whose tangents all packed to none digests
        /// as the box that brought none — which is every vanilla scene, and so the baselines on
        /// record hold.
        TEST(RtxSceneDigestTest, aTangentMovesTheNormalsAndMeshesColumnsAndNoTangentMovesNothing)
        {
            const auto [bare, bareParts] = digestsOfTangents({});

            std::array<osg::Vec4f, 8> none{};
            const auto [nothing, nothingParts] = digestsOfTangents(none);
            EXPECT_EQ(nothing, bare) << "tangents of no length moved the scene";
            EXPECT_EQ(nothingParts, bareParts) << "tangents of no length moved a column";

            std::array<osg::Vec4f, 8> alongX;
            alongX.fill(osg::Vec4f(1.0f, 0.0f, 0.0f, 1.0f));
            const auto [tangent, tangentParts] = digestsOfTangents(alongX);
            EXPECT_NE(tangent, bare);
            for (std::size_t at = 0; at < bareParts.size(); ++at)
            {
                const auto part = static_cast<ScenePart>(at);
                EXPECT_EQ(tangentParts[at] != bareParts[at], part == ScenePart::Normals || part == ScenePart::Meshes)
                    << nameOf(part);
            }

            // The handedness alone is a change as well.
            std::array<osg::Vec4f, 8> flipped;
            flipped.fill(osg::Vec4f(1.0f, 0.0f, 0.0f, -1.0f));
            const auto [other, otherParts] = digestsOfTangents(flipped);
            EXPECT_NE(other, tangent);
            EXPECT_NE(otherParts[static_cast<std::size_t>(ScenePart::Normals)],
                tangentParts[static_cast<std::size_t>(ScenePart::Normals)]);
        }

        /// One sprite under one emitter that adds where `additive` and falls where `falls`,
        /// digested part by part.
        ScenePartDigests partsOfPlume(const bool additive, const bool falls)
        {
            Rtx::SceneDesc scene;
            const Rtx::Index texture = scene.textures().add(VFS::Path::NormalizedView("textures/puff.dds"));
            const std::array sprites{ Rtx::Sprite{ .mPosition = osg::Vec3f(1.0f, 2.0f, 3.0f),
                .mRadius = 4.0f,
                .mColour = osg::Vec3f(1.0f, 1.0f, 1.0f),
                .mAlpha = 1.0f } };
            scene.addEmitter(sprites, texture, additive, 0.0f, Rtx::sNoIndex, falls);

            return digestParts(scene);
        }

        /// **An emitter's flag word moves the emitters column.** The table is hashed whole, so a
        /// bit that changed what the trace does with a plume — whether it adds, whether a roof
        /// keeps it off — has to be a change the report can see, and a sprite's own row names its
        /// emitter, which is the sprites column's to see.
        TEST(RtxSceneDigestTest, anEmittersFlagsMoveTheEmittersColumn)
        {
            const ScenePartDigests smoke = partsOfPlume(false, false);
            EXPECT_EQ(smoke, partsOfPlume(false, false)) << "one plume built twice";

            const ScenePartDigests flame = partsOfPlume(true, false);
            const ScenePartDigests rain = partsOfPlume(false, true);
            for (std::size_t at = 0; at < smoke.size(); ++at)
            {
                const auto part = static_cast<ScenePart>(at);
                EXPECT_EQ(smoke[at] != flame[at], part == ScenePart::Emitters) << nameOf(part) << " under a flame";
                EXPECT_EQ(smoke[at] != rain[at], part == ScenePart::Emitters) << nameOf(part) << " under rain";
            }
        }

        /// One quad on a one-bone rig whose single influence weighs `weight`, posed with its bone
        /// `up` units high, digested part by part.
        ScenePartDigests partsOfSkin(const float weight, const float up)
        {
            Rtx::SceneDesc scene;

            const std::array<std::uint32_t, 4> runs{ 1u, 1u, 1u, 1u };
            const std::array influences{ Rtx::Shaders::GpuInfluence{ .mBone = 0, .mWeight = weight } };

            const std::array positions{ osg::Vec3f(), osg::Vec3f(1.0f, 0.0f, 0.0f), osg::Vec3f(1.0f, 1.0f, 0.0f),
                osg::Vec3f(0.0f, 1.0f, 0.0f) };
            const std::array<std::uint32_t, 6> indices{ 0, 1, 2, 0, 2, 3 };
            const Rtx::Index quad = scene
                                        .addMesh(Rtx::MeshArrays{ .mPositions = positions, .mIndices = indices }, {},
                                            Rtx::RigSpec{ .mRuns = runs, .mInfluences = influences, .mBones = 1 })
                                        .mMesh;

            const std::array bones{ Rtx::toGpuBone(osg::Matrixf::translate(0.0f, 0.0f, up)) };
            std::vector<Rtx::PoseWord> words;
            Rtx::packBones(bones, words);
            // One reach whatever the pose, because the reach is the mesh's own row and so the
            // meshes column's: what this asks is what the words move on their own.
            scene.pose(quad, words, osg::BoundingBoxf(osg::Vec3f(), osg::Vec3f(1.0f, 1.0f, 10.0f)));

            return digestParts(scene);
        }

        /// **A pose moves the poses column alone, and a skin the deformers column alone.** The two
        /// are one table and one buffer in the scene; a column apiece is what lets a pair say
        /// whether the actor moved or the actor changed.
        TEST(RtxSceneDigestTest, aPoseAndASkinEachMoveTheirOwnColumn)
        {
            const ScenePartDigests one = partsOfSkin(1.0f, 5.0f);
            EXPECT_EQ(one, partsOfSkin(1.0f, 5.0f)) << "one scene built twice";

            const ScenePartDigests posed = partsOfSkin(1.0f, 7.0f);
            const ScenePartDigests reskinned = partsOfSkin(0.5f, 5.0f);
            for (std::size_t at = 0; at < one.size(); ++at)
            {
                const auto part = static_cast<ScenePart>(at);
                EXPECT_EQ(one[at] != posed[at], part == ScenePart::Poses) << nameOf(part) << " under another pose";
                EXPECT_EQ(one[at] != reskinned[at], part == ScenePart::Deformers)
                    << nameOf(part) << " under another skin";
            }
        }

        /// **A digester hashes the vertex tables where they moved and nowhere else**, and answers
        /// the same words as a digest made from nothing. A frame with nothing new pays no rehash
        /// of forty megabytes; a mesh added costs one, a release none, and the next mesh into the
        /// hole it left one more.
        TEST(RtxSceneDigestTest, aDigesterHashesTheVertexTablesOnceForEveryChangeToThem)
        {
            Rtx::SceneDesc scene;
            fillBoxes(scene, false, 0.0f, 0.0f);

            SceneDigester digester;
            const ScenePartDigests first = digester.digest(scene);
            EXPECT_EQ(digester.getVertexHashes(), 1u);
            EXPECT_EQ(first, digestParts(scene)) << "a digester and a digest from nothing disagree";

            // The same scene again, and a pose of nothing: the cache answers.
            EXPECT_EQ(digester.digest(scene), first);
            EXPECT_EQ(digester.getVertexHashes(), 1u) << "an unchanged table was hashed again";

            // A placement moved touches no vertex table; the instances column moves alone.
            scene.placements().move(0, osg::Matrixf::translate(1.0f, 0.0f, 0.0f));
            const ScenePartDigests moved = digester.digest(scene);
            EXPECT_EQ(digester.getVertexHashes(), 1u);
            EXPECT_EQ(moved[static_cast<std::size_t>(ScenePart::Positions)],
                first[static_cast<std::size_t>(ScenePart::Positions)]);
            EXPECT_NE(moved[static_cast<std::size_t>(ScenePart::Instances)],
                first[static_cast<std::size_t>(ScenePart::Instances)]);

            // A mesh added is a revision, and the vertex columns move with it.
            const Rtx::Index third = addBox(scene, true, 2.0f);
            const ScenePartDigests grown = digester.digest(scene);
            EXPECT_EQ(digester.getVertexHashes(), 2u);
            EXPECT_NE(grown[static_cast<std::size_t>(ScenePart::Positions)],
                moved[static_cast<std::size_t>(ScenePart::Positions)]);
            EXPECT_EQ(grown, digestParts(scene));

            // A release leaves the bytes where they were — a freed run is a hole, and the table
            // keeps its length — so the cache still answers, and answers what a digest from nothing
            // says. The mesh row it emptied moves the meshes column and nothing else.
            const std::array keepTwo{ Rtx::Index{ 0 }, Rtx::Index{ 1 } };
            const std::array keepMaterials{ Rtx::Index{ 0 } };
            ASSERT_TRUE(scene.release(keepTwo, keepMaterials));
            const ScenePartDigests released = digester.digest(scene);
            EXPECT_EQ(digester.getVertexHashes(), 2u) << "a release wrote no vertex and was hashed for it";
            EXPECT_EQ(released, digestParts(scene));
            EXPECT_NE(released[static_cast<std::size_t>(ScenePart::Meshes)],
                grown[static_cast<std::size_t>(ScenePart::Meshes)]);

            // And the next mesh to land in the hole rewrites its bytes under a new revision.
            EXPECT_EQ(addBox(scene, false, 3.0f), third) << "the freed slot is the one handed out";
            const ScenePartDigests refilled = digester.digest(scene);
            EXPECT_EQ(digester.getVertexHashes(), 3u);
            EXPECT_NE(refilled[static_cast<std::size_t>(ScenePart::Positions)],
                released[static_cast<std::size_t>(ScenePart::Positions)]);
            EXPECT_EQ(refilled, digestParts(scene));
        }
    }
}
