#include <algorithm>
#include <array>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/error.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/spritelight.hpp>

namespace Rtx
{
    namespace
    {
        const std::array sQuadPositions{
            osg::Vec3f(0.0f, 0.0f, 0.0f),
            osg::Vec3f(1.0f, 0.0f, 0.0f),
            osg::Vec3f(1.0f, 1.0f, 0.0f),
            osg::Vec3f(0.0f, 1.0f, 0.0f),
        };

        constexpr std::array<std::uint32_t, 6> sQuadIndices{ 0, 1, 2, 0, 2, 3 };

        /// What a news list names, sorted, so a set can be compared without depending on the order
        /// the sweep happened to walk its table in.
        std::vector<Index> sorted(std::span<const Index> slots)
        {
            std::vector<Index> copy(slots.begin(), slots.end());
            std::sort(copy.begin(), copy.end());
            return copy;
        }

        TEST(RtxSceneDescTest, aMeshRemembersWhereItsVerticesWent)
        {
            SceneDesc scene;

            const Index first = scene.addMesh(sQuadPositions, {}, {}, sQuadIndices);
            const Index second = scene.addMesh(sQuadPositions, {}, {}, sQuadIndices);

            EXPECT_EQ(first, 0u);
            EXPECT_EQ(second, 1u);

            // Two quads: 8 vertices and 12 indices in the shared buffers, the second mesh starting
            // where the first left off.
            EXPECT_EQ(scene.getPositions().size(), 8u);
            EXPECT_EQ(scene.getIndices().size(), 12u);
            EXPECT_EQ(scene.getMeshes()[1].mVertexOffset, 4u);
            EXPECT_EQ(scene.getMeshes()[1].mIndexOffset, 6u);

            EXPECT_EQ(scene.getMeshPositions(second)[2], osg::Vec3f(1.0f, 1.0f, 0.0f));
            EXPECT_EQ(scene.getMeshIndices(second)[5], 3u);

            // And whether the caller found it doubled for its back, which the scene keeps and
            // never works out for itself.
            EXPECT_FALSE(scene.getMeshes()[first].mSheet);
            EXPECT_TRUE(scene.getMeshes()[scene.addMesh(sQuadPositions, {}, {}, sQuadIndices, true)].mSheet);
        }

        /// A mesh without normals or texture coordinates must still leave the attribute buffers as
        /// long as the position buffer, or every vertex after it reads someone else's normal.
        TEST(RtxSceneDescTest, theAttributeBuffersStayParallelWhenAMeshBringsNoAttributes)
        {
            SceneDesc scene;

            const std::array sNormals{
                osg::Vec3f(0.0f, 0.0f, 1.0f),
                osg::Vec3f(0.0f, 0.0f, 1.0f),
                osg::Vec3f(0.0f, 0.0f, 1.0f),
                osg::Vec3f(0.0f, 0.0f, 1.0f),
            };

            scene.addMesh(sQuadPositions, {}, {}, sQuadIndices);
            const Index withNormals = scene.addMesh(sQuadPositions, sNormals, {}, sQuadIndices);

            ASSERT_EQ(scene.getNormals().size(), scene.getPositions().size());
            ASSERT_EQ(scene.getTexCoords().size(), scene.getPositions().size());

            const MeshRange& range = scene.getMeshes()[withNormals];
            EXPECT_EQ(scene.getNormals()[range.mVertexOffset], osg::Vec3f(0.0f, 0.0f, 1.0f));
            EXPECT_EQ(scene.getNormals()[0], osg::Vec3f(0.0f, 0.0f, 0.0f));
        }

        TEST(RtxSceneDescTest, aTextureIsAddedOnceHoweverOftenItIsAskedFor)
        {
            SceneDesc scene;

            constexpr VFS::Path::NormalizedView stone("textures/tx_stone_01.dds");
            constexpr VFS::Path::NormalizedView wood("textures/tx_wood_01.dds");

            EXPECT_EQ(scene.addTexture(stone), 0u);
            EXPECT_EQ(scene.addTexture(wood), 1u);
            EXPECT_EQ(scene.addTexture(stone), 0u);
            EXPECT_EQ(scene.getTextures().size(), 2u);
        }

        TEST(RtxSceneDescTest, theCountsAreWhatTheBuffersHold)
        {
            SceneDesc scene;
            scene.addMesh(sQuadPositions, {}, {}, sQuadIndices);
            scene.addMesh(sQuadPositions, {}, {}, sQuadIndices);

            EXPECT_EQ(scene.getTriangleCount(), 4u);
            EXPECT_EQ(scene.getMeshes()[0].getTriangleCount(), 2u);

            // 8 positions and 8 normals at 12 bytes, 8 texture coordinates at 8, 12 indices at 4.
            EXPECT_EQ(scene.getGeometryBytes(), 8u * 12u + 8u * 12u + 8u * 8u + 12u * 4u);
        }

        /// The cutoff a material is traced against, and which materials get traced against one.
        ///
        /// The blended case is the load-bearing one: Morrowind's foliage is drawn with
        /// `NiAlphaProperty` and no alpha test, so a renderer that only honoured the tested mode
        /// would find nothing to cut out. A blend that *did* name a threshold keeps its own.
        TEST(RtxSceneDescTest, onlyAMaterialWithAMaskToReadIsTracedAsACutout)
        {
            constexpr Index texture = 3;

            const Material opaque{ .mDiffuse = texture };
            EXPECT_EQ(opaque.getAlphaCutoff(), 0.0f);
            EXPECT_FALSE(opaque.isCutout());

            const Material tested{ .mDiffuse = texture, .mAlphaRef = 0.3f, .mAlphaMode = AlphaMode::Cutout };
            EXPECT_EQ(tested.getAlphaCutoff(), 0.3f);
            EXPECT_TRUE(tested.isCutout());

            const Material blended{ .mDiffuse = texture, .mAlphaMode = AlphaMode::Blend };
            EXPECT_EQ(blended.getAlphaCutoff(), 0.5f);
            EXPECT_TRUE(blended.isCutout());

            const Material blendedWithRef{ .mDiffuse = texture, .mAlphaRef = 0.8f, .mAlphaMode = AlphaMode::Blend };
            EXPECT_EQ(blendedWithRef.getAlphaCutoff(), 0.8f);

            // The mask lives in the diffuse map's alpha, so a cutoff with no map to read it from is
            // not a cutout — and marking it one would cost traversal a candidate loop that could
            // only ever say yes.
            const Material untextured{ .mAlphaMode = AlphaMode::Blend };
            EXPECT_EQ(untextured.getAlphaCutoff(), 0.5f);
            EXPECT_FALSE(untextured.isCutout());
        }

        /// A leaf card and a pane of glass carry the same alpha mode, and the material's own alpha is
        /// what tells them apart.
        ///
        /// **The mode says nothing about it**, because Morrowind keeps its foliage under
        /// `NiAlphaProperty`: a leaf is fully opaque wherever its painted mask is, and a pane is
        /// translucent everywhere. The two want opposite answers from traversal — a mask averaged
        /// over the ray cone and tested is right for the leaf and turns the pane solid; light
        /// attenuated as it passes is right for the pane and turns the leaf to gauze — so nothing may
        /// act on the mode alone.
        ///
        /// `NiMaterialProperty` records that alpha and `NifOsg::AlphaController` animates it, so a
        /// surface can cross this line while the game runs.
        TEST(RtxSceneDescTest, theMaterialsOwnAlphaIsWhatTellsAPaneOfGlassFromALeaf)
        {
            constexpr Index texture = 3;

            const Material leaf{ .mDiffuse = texture, .mAlphaMode = AlphaMode::Blend };
            EXPECT_FALSE(leaf.isTranslucent()) << "a painted mask on an opaque material";
            EXPECT_TRUE(leaf.isCutout()) << "and it keeps the branch it has";

            const Material pane{ .mDiffuse = texture,
                .mDiffuseColour = osg::Vec4f(1.0f, 1.0f, 1.0f, 0.3f),
                .mAlphaMode = AlphaMode::Blend };
            EXPECT_TRUE(pane.isTranslucent());

            // The mode is half of it: a faded material the content never asked to blend is drawn as
            // it was authored, and a cutout stays a cutout however faint its own colour is.
            const Material faded{ .mDiffuse = texture, .mDiffuseColour = osg::Vec4f(1.0f, 1.0f, 1.0f, 0.3f) };
            EXPECT_FALSE(faded.isTranslucent()) << "opaque mode, whatever the colour says";

            const Material tested{ .mDiffuse = texture,
                .mDiffuseColour = osg::Vec4f(1.0f, 1.0f, 1.0f, 0.3f),
                .mAlphaRef = 0.3f,
                .mAlphaMode = AlphaMode::Cutout };
            EXPECT_FALSE(tested.isTranslucent()) << "a mask the content asked to test is a mask";
        }

        /// A deformed mesh keeps its slot and its topology, and says so.
        ///
        /// The second mesh is what makes the test worth running: an update that wrote at the wrong
        /// offset would land in a neighbour, and with one mesh in the scene there is no neighbour to
        /// land in.
        TEST(RtxSceneDescTest, aDeformedMeshKeepsItsSlotAndNamesItselfOnce)
        {
            const std::array sNormals{
                osg::Vec3f(0.0f, 0.0f, 1.0f),
                osg::Vec3f(0.0f, 0.0f, 1.0f),
                osg::Vec3f(0.0f, 0.0f, 1.0f),
                osg::Vec3f(0.0f, 0.0f, 1.0f),
            };

            SceneDesc scene;
            const Index still = scene.addMesh(sQuadPositions, sNormals, {}, sQuadIndices);
            const Index moving = scene.addMesh(sQuadPositions, sNormals, {}, sQuadIndices);

            EXPECT_TRUE(scene.getDeformed().empty()) << "nothing has deformed yet";

            // The same quad a unit further along z, facing the other way.
            std::array<osg::Vec3f, 4> posed = sQuadPositions;
            for (osg::Vec3f& vertex : posed)
                vertex.z() += 1.0f;

            const std::array sPosedNormals{
                osg::Vec3f(0.0f, 0.0f, -1.0f),
                osg::Vec3f(0.0f, 0.0f, -1.0f),
                osg::Vec3f(0.0f, 0.0f, -1.0f),
                osg::Vec3f(0.0f, 0.0f, -1.0f),
            };

            scene.updateMesh(moving, posed, sPosedNormals);
            scene.updateMesh(moving, posed, sPosedNormals);

            ASSERT_EQ(scene.getDeformed().size(), 1u) << "twice in a frame is one structure to build";
            EXPECT_EQ(scene.getDeformed()[0], moving);

            // Eight vertices still, at the same offsets: an update is not an append.
            EXPECT_EQ(scene.getPositions().size(), 8u);
            EXPECT_EQ(scene.getIndices().size(), 12u);
            EXPECT_EQ(scene.getMeshes()[moving].mVertexOffset, 4u);

            EXPECT_EQ(scene.getMeshPositions(moving)[2], osg::Vec3f(1.0f, 1.0f, 1.0f));
            EXPECT_EQ(scene.getNormals()[scene.getMeshes()[moving].mVertexOffset], osg::Vec3f(0.0f, 0.0f, -1.0f));
            EXPECT_EQ(scene.getMeshIndices(moving)[5], 3u) << "topology is what does not change";

            // And the mesh beside it is untouched, which is the offset arithmetic being right rather
            // than merely being applied.
            EXPECT_EQ(scene.getMeshPositions(still)[2], osg::Vec3f(1.0f, 1.0f, 0.0f));
            EXPECT_EQ(scene.getNormals()[scene.getMeshes()[still].mVertexOffset], osg::Vec3f(0.0f, 0.0f, 1.0f));

            // The list is a frame's worth, so it goes when the frame's placements do.
            scene.clearPlacement();
            EXPECT_TRUE(scene.getDeformed().empty());
            EXPECT_EQ(scene.getMeshes().size(), 2u) << "clearing where things are keeps what they are";
        }

        /// An emitter's sphere is derived from the sprites rather than passed in, so the rejection
        /// test a ray makes and the sprites it would then walk cannot disagree about where they are.
        ///
        /// **Off the box and not off the mean**, which the lopsided arrangement here is chosen to
        /// prove: two sprites sit at the origin and one at four along x, so the mean is at 4/3 and
        /// the box's centre at 2. From the box the reach is 2 + 1 = 3 either way; from the mean it
        /// would have to be 8/3 + 1 = 3.67 to hold the far one, a sphere 22% wider for the same
        /// three particles.
        TEST(RtxSceneDescTest, anEmitterCarriesItsSpritesAndTheSphereThatHoldsThem)
        {
            SceneDesc scene;
            const Index texture = scene.addTexture(VFS::Path::NormalizedView("textures/tx_fire_00.dds"));

            // The bake of the texture's alpha sits in the same table, which is why the count of
            // textures at the end is two.
            const Index lighting
                = scene.addBakedTexture(SpriteLightMap::keyFor(VFS::Path::NormalizedView("textures/tx_fire_00.dds")));

            const std::array sPlume{
                Sprite{ .mPosition = osg::Vec3f(0.0f, 0.0f, 0.0f), .mRadius = 1.0f },
                Sprite{ .mPosition = osg::Vec3f(0.0f, 0.0f, 0.0f), .mRadius = 1.0f },
                Sprite{ .mPosition = osg::Vec3f(4.0f, 0.0f, 0.0f), .mRadius = 1.0f },
            };
            scene.addEmitter(sPlume, texture, true, osg::Vec3f(), osg::Vec3f(), lighting);

            ASSERT_EQ(scene.getEmitters().size(), 1u);
            const SpriteEmitter& plume = scene.getEmitters().front();
            EXPECT_EQ(plume.mCentre, osg::Vec3f(2.0f, 0.0f, 0.0f));
            EXPECT_FLOAT_EQ(plume.mReach, 3.0f);
            EXPECT_EQ(plume.mFirst, 0u);
            EXPECT_EQ(plume.mCount, 3u);
            EXPECT_EQ(plume.mTexture, texture);
            EXPECT_EQ(plume.mLighting, lighting);
            EXPECT_TRUE(plume.mAdditive);

            // An emitter with nothing alive in it is not an emitter, and the next one that has
            // something starts where the first left off rather than where a placeholder would have.
            scene.addEmitter({}, texture, false);
            EXPECT_EQ(scene.getEmitters().size(), 1u);

            const std::array sSmoke{ Sprite{ .mPosition = osg::Vec3f(0.0f, 0.0f, 10.0f), .mRadius = 2.0f } };
            scene.addEmitter(sSmoke, texture, false);

            ASSERT_EQ(scene.getEmitters().size(), 2u);
            EXPECT_EQ(scene.getEmitters()[1].mFirst, 3u);
            EXPECT_EQ(scene.getEmitters()[1].mCount, 1u);
            EXPECT_FALSE(scene.getEmitters()[1].mAdditive)
                << "the blend the file asked for is what tells the two apart";
            EXPECT_EQ(scene.getEmitters()[1].mLighting, sNoIndex) << "an emitter with no bake is lit as a card";
            EXPECT_EQ(scene.getSprites().size(), 4u);
            EXPECT_EQ(scene.getSprites()[3].mPosition, osg::Vec3f(0.0f, 0.0f, 10.0f));

            // A frame's worth, so they go when the frame's placements do — and the texture they name
            // stays, because the array it indexes was uploaded when the scene was built.
            scene.clearPlacement();
            EXPECT_TRUE(scene.getEmitters().empty());
            EXPECT_TRUE(scene.getSprites().empty());
            EXPECT_EQ(scene.getTextures().size(), 2u);
        }

        /// A quad that hangs in the world reaches further than its own width, and its sphere knows.
        ///
        /// **Morrowind's rain is why `osgParticle` has a `FIXED` mode at all.** A billboard's axes
        /// are the screen's and it is a disc of one radius; a fixed one's are authored, and its
        /// *lengths* are the shape — rain's X is squashed to a tenth against a Y pointing straight
        /// down, which is a falling streak rather than a round drop.
        ///
        /// The reach has to be measured on that, and it is the one thing about the mode that a
        /// bounding sphere cannot guess: a streak ten times as tall as it is wide, measured on the
        /// width, is cut off nine tenths of the way up.
        TEST(RtxSceneDescTest, aFixedSpriteReachesByItsOwnAxesAndAnEyeFacingOneByItsRadius)
        {
            SceneDesc scene;
            const Index texture = scene.addTexture(VFS::Path::NormalizedView("textures/tx_raindrop_01.dds"));

            const std::array one{ Sprite{ .mPosition = osg::Vec3f(), .mRadius = 10.0f } };

            // Facing the eye: a disc, and the reach is the radius.
            scene.addEmitter(one, texture, false);
            ASSERT_EQ(scene.getEmitters().size(), 1u);
            EXPECT_FALSE(scene.getEmitters()[0].isFixed()) << "two zero axes is a billboard";
            EXPECT_FLOAT_EQ(scene.getEmitters()[0].mReach, 10.0f);

            // Morrowind's own rain axes. The quad runs `+-0.1 * 10` across and `+-1 * 10` down, so
            // its corner is `|(0.1, 0, -1)| * 10 = 10.0499` from the middle — and that, not the ten,
            // is what has to fit in the sphere.
            const osg::Vec3f across(0.1f, 0.0f, 0.0f);
            const osg::Vec3f upward(0.0f, 0.0f, -1.0f);
            scene.addEmitter(one, texture, false, across, upward);

            ASSERT_EQ(scene.getEmitters().size(), 2u);
            const SpriteEmitter& rain = scene.getEmitters()[1];
            EXPECT_TRUE(rain.isFixed());
            EXPECT_EQ(rain.mAcross, across) << "carried as authored, because the length is the shape";
            EXPECT_EQ(rain.mUpward, upward);
            EXPECT_NEAR(rain.mReach, 10.0499f, 1e-3f);
            EXPECT_GT(rain.mReach, 10.0f) << "further than the radius alone would have reached";

            // **And an axis of nothing is not an orientation.** A system that named only one of them
            // is a billboard, which is what the zero state has to mean for a default to be safe.
            scene.addEmitter(one, texture, false, across, osg::Vec3f());
            ASSERT_EQ(scene.getEmitters().size(), 3u);
            EXPECT_FALSE(scene.getEmitters()[2].isFixed());
        }

        /// A triangle, so that a mesh beside the quads has a length of its own to be packed against.
        const std::array sTrianglePositions{
            osg::Vec3f(0.0f, 0.0f, 5.0f),
            osg::Vec3f(1.0f, 0.0f, 5.0f),
            osg::Vec3f(0.0f, 1.0f, 5.0f),
        };

        constexpr std::array<std::uint32_t, 3> sTriangleIndices{ 0, 1, 2 };

        /// The same quad lifted to `z`, so a mesh can be told apart by what came back out of it.
        std::array<osg::Vec3f, 4> quadAt(float z)
        {
            std::array<osg::Vec3f, 4> lifted = sQuadPositions;
            for (osg::Vec3f& vertex : lifted)
                vertex.z() = z;

            return lifted;
        }

        /// A freed mesh keeps its index, and the room it held goes back for the next mesh to take.
        ///
        /// Hand-counted throughout. Three meshes of 4, 3 and 4 vertices sit at vertex offsets 0, 4
        /// and 7 and index offsets 0, 6 and 9. Freeing the middle one moves nothing: the third is
        /// still index 2 at vertex 7, and the hole at vertex 4 is three vertices and three indices
        /// wide — which is exactly a triangle, and exactly what the next triangle takes.
        ///
        /// **Not compacting is the whole point.** Closing the gap renames every mesh above it, and a
        /// mesh index is what every bottom-level acceleration structure in the world is named by, so
        /// a cell boundary cost a full rebuild.
        TEST(RtxSceneDescTest, aFreedMeshKeepsItsSlotAndTheNextThatFitsTakesIt)
        {
            SceneDesc scene;
            const std::array quads{ quadAt(0.0f), quadAt(2.0f) };
            const Index first = scene.addMesh(quads[0], {}, {}, sQuadIndices);
            const Index middle = scene.addMesh(sTrianglePositions, {}, {}, sTriangleIndices);
            const Index last = scene.addMesh(quads[1], {}, {}, sQuadIndices);

            ASSERT_EQ(scene.getPositions().size(), 11u);
            ASSERT_EQ(scene.getIndices().size(), 15u);
            ASSERT_EQ(scene.getMeshes()[last].mVertexOffset, 7u);

            const std::uint64_t was = scene.getStructureRevision();
            const std::array keep{ first, last };
            const std::array<Index, 0> noMaterials{};
            ASSERT_TRUE(scene.release(keep, noMaterials));

            // Nothing moved, nothing shrank, and every index still means what it meant.
            EXPECT_EQ(scene.getMeshes().size(), 3u);
            EXPECT_EQ(scene.getPositions().size(), 11u);
            EXPECT_EQ(scene.getIndices().size(), 15u);
            EXPECT_EQ(scene.getMeshes()[last].mVertexOffset, 7u);
            EXPECT_EQ(scene.getMeshPositions(first)[0].z(), 0.0f);
            EXPECT_EQ(scene.getMeshPositions(last)[0].z(), 2.0f);

            // The freed one describes nothing until something takes it, so a backend that walks the
            // table builds a structure over no triangles rather than over somebody else's.
            EXPECT_EQ(scene.getMeshes()[middle].mVertexCount, 0u);
            EXPECT_EQ(scene.getMeshes()[middle].mIndexCount, 0u);

            EXPECT_EQ(scene.getStructureRevision(), was)
                << "nothing arrived, so nothing built from these indices is out of date";

            // **The sweep names the slot it gave up, and it stops being an arrival by naming it.**
            // Nothing has been handed over, so all three are still spoken for — two as arrivals and
            // the third as a departure, never as both.
            EXPECT_EQ(sorted(scene.getFreedMeshes()), (std::vector<Index>{ middle }));
            EXPECT_EQ(sorted(scene.getArrivedMeshes()), (std::vector<Index>{ first, last }));

            // A triangle fits the hole exactly and takes it back, at the index and the offset the
            // old one had.
            const Index moved = scene.addMesh(sTrianglePositions, {}, {}, sTriangleIndices);
            EXPECT_EQ(moved, middle);
            EXPECT_EQ(scene.getMeshes()[moved].mVertexOffset, 4u);
            EXPECT_EQ(scene.getMeshes()[moved].mVertexCount, 3u);
            EXPECT_EQ(scene.getPositions().size(), 11u) << "a reused slot appended";
            EXPECT_GT(scene.getStructureRevision(), was) << "a slot taken over holds different geometry";

            // And the last mesh is still where it was, which a compaction is what would break.
            EXPECT_EQ(scene.getMeshPositions(last)[0].z(), 2.0f);

            // **Taking the slot back moves it the other way**, which is what lets a backend apply
            // the two lists in either order: this slot is built and not then destroyed, whichever
            // half it does first.
            EXPECT_EQ(sorted(scene.getArrivedMeshes()), (std::vector<Index>{ first, moved, last }));
            EXPECT_TRUE(scene.getFreedMeshes().empty()) << "a slot taken back was still reported as gone";

            scene.clearArrivals();
            EXPECT_TRUE(scene.getArrivedMeshes().empty());
            EXPECT_TRUE(scene.getFreedMeshes().empty());
        }

        /// A mesh arriving is told from a texture arriving, and a reused slot counts as an arrival.
        ///
        /// **The guard a backend builds on, and getting it wrong crashes.** `VulkanRenderer` rebuilds
        /// its acceleration structures when a mesh arrives and not when a texture does, and it used
        /// to ask the table's *size* — which cannot see a freed slot taken over by something else.
        /// A skinned body landing in one was then refitted into a bottom-level structure that had
        /// never been made for it, which is a build into a null handle.
        TEST(RtxSceneDescTest, aMeshArrivingIsToldFromATextureArrivingAndAReusedSlotIsAnArrival)
        {
            SceneDesc scene;
            const Index slot = scene.addMesh(sQuadPositions, {}, {}, sQuadIndices);

            const std::uint64_t meshes = scene.getMeshRevision();
            const std::uint64_t structure = scene.getStructureRevision();

            // A texture is an upload, not a structure to build.
            scene.addTexture(VFS::Path::NormalizedView("textures/tx_stone.dds"));
            EXPECT_EQ(scene.getMeshRevision(), meshes) << "a texture asked for the structures to be built again";
            EXPECT_GT(scene.getStructureRevision(), structure);

            // The slot comes back and is taken over. The table is the same size it was, and what is
            // in it is not.
            ASSERT_TRUE(scene.release({}, {}));
            EXPECT_EQ(scene.getMeshRevision(), meshes) << "a cell leaving asked for the structures to be built again";

            EXPECT_EQ(scene.addMesh(sTrianglePositions, {}, {}, sTriangleIndices), slot);
            EXPECT_EQ(scene.getMeshes().size(), 1u) << "the table grew, so a size test would have caught this anyway";
            EXPECT_GT(scene.getMeshRevision(), meshes) << "a slot taken over went unnoticed";
        }

        /// Room given back is reused, and a mesh with nowhere to fit appends rather than being
        /// refused.
        ///
        /// **Two meshes freed side by side are one hole and not two.** A twelve-vertex mesh arrived,
        /// then a four; both go, and what is left is a single run of twelve vertices at zero rather
        /// than a pair that between them can hold nothing bigger than the larger. That is what a
        /// cell boundary is — thousands of runs laid end to end, released together — and it is why
        /// the geometry buffers stop growing once a player has travelled a while.
        ///
        /// Hand-counted: 8, 4 and 4 vertices at offsets 0, 8 and 12, and 12, 6 and 6 indices at 0,
        /// 12 and 18. Keeping only the last leaves one vertex hole of twelve at zero and one index
        /// hole of eighteen at zero.
        TEST(RtxSceneDescTest, roomGivenBackIsMergedAndReused)
        {
            SceneDesc scene;

            // Eight vertices and twelve indices, which is two quads' worth in one mesh.
            std::vector<osg::Vec3f> big;
            std::vector<std::uint32_t> bigIndices;
            for (int copy = 0; copy < 2; ++copy)
            {
                for (const osg::Vec3f& vertex : quadAt(static_cast<float>(copy)))
                    big.push_back(vertex);

                for (const std::uint32_t index : sQuadIndices)
                    bigIndices.push_back(index + static_cast<std::uint32_t>(copy) * 4u);
            }

            const Index roomy = scene.addMesh(big, {}, {}, bigIndices);
            const Index snug = scene.addMesh(sQuadPositions, {}, {}, sQuadIndices);
            const Index kept = scene.addMesh(sQuadPositions, {}, {}, sQuadIndices);

            ASSERT_EQ(scene.getMeshes()[roomy].mVertexOffset, 0u);
            ASSERT_EQ(scene.getMeshes()[snug].mVertexOffset, 8u);
            ASSERT_EQ(scene.getMeshes()[kept].mVertexOffset, 12u);

            const std::array keep{ kept };
            ASSERT_TRUE(scene.release(keep, {}));

            // Exactly the two that went, once each. Sorted, because which way a sweep walks its
            // table is not something a backend should have to know.
            EXPECT_EQ(sorted(scene.getFreedMeshes()), (std::vector<Index>{ roomy, snug }));

            const std::size_t vertices = scene.getPositions().size();
            ASSERT_EQ(vertices, 16u);

            // The quad takes the front of the merged hole and leaves eight vertices behind it.
            const Index quad = scene.addMesh(sQuadPositions, {}, {}, sQuadIndices);
            EXPECT_EQ(scene.getMeshes()[quad].mVertexOffset, 0u);

            // **Which is what the eight-vertex mesh then fits into.** Unmerged, the two holes were
            // eight and four and the four had just been spent, so this would have appended.
            const Index again = scene.addMesh(big, {}, {}, bigIndices);
            EXPECT_EQ(scene.getMeshes()[again].mVertexOffset, 4u);
            EXPECT_EQ(scene.getPositions().size(), vertices) << "a mesh that fitted a hole appended anyway";

            // Both freed slots have been taken, in the order they were given back.
            EXPECT_EQ(quad, snug);
            EXPECT_EQ(again, roomy);

            // Nothing fits now, so this one goes on the end.
            EXPECT_EQ(scene.addMesh(big, {}, {}, bigIndices), 3u);
            EXPECT_GT(scene.getPositions().size(), vertices);
        }

        /// A slot taken over holds its own attributes and none of its predecessor's.
        ///
        /// **The one way a reused slot can be quietly wrong.** A mesh that brings no normals is
        /// given zeroes on a fresh slot because the buffer was grown for it; on a reused one the
        /// room already holds whatever the last tenant put there, and a surface lit by somebody
        /// else's normals looks lit rather than looking broken.
        TEST(RtxSceneDescTest, aReusedSlotDoesNotInheritTheAttributesOfWhatStoodInIt)
        {
            SceneDesc scene;

            const std::array<osg::Vec3f, 4> normals{ osg::Vec3f(1.0f, 0.0f, 0.0f), osg::Vec3f(1.0f, 0.0f, 0.0f),
                osg::Vec3f(1.0f, 0.0f, 0.0f), osg::Vec3f(1.0f, 0.0f, 0.0f) };
            const std::array<osg::Vec2f, 4> uvs{ osg::Vec2f(0.5f, 0.5f), osg::Vec2f(0.5f, 0.5f), osg::Vec2f(0.5f, 0.5f),
                osg::Vec2f(0.5f, 0.5f) };

            const Index slot = scene.addMesh(sQuadPositions, normals, uvs, sQuadIndices);
            ASSERT_EQ(scene.getNormals()[scene.getMeshes()[slot].mVertexOffset], osg::Vec3f(1.0f, 0.0f, 0.0f));

            ASSERT_TRUE(scene.release({}, {}));
            EXPECT_EQ(scene.addMesh(sQuadPositions, {}, {}, sQuadIndices), slot);

            EXPECT_EQ(scene.getNormals()[scene.getMeshes()[slot].mVertexOffset], osg::Vec3f())
                << "the slot kept the last tenant's normals";
            EXPECT_EQ(scene.getTexCoords()[0], osg::Vec2f());
        }

        /// A material frees its slot, and the layer run and masks behind it come back too.
        ///
        /// Hand-counted: three materials, of which the first and last are terrain with one and two
        /// layers. The layers sit at 0, 1 and 2 and their masks at 0 and 4, nine weights of the
        /// second sitting behind four of the first. Freeing the first leaves a one-long hole in the
        /// layer table and a four-long one in the masks, and the next chunk of the same shape lands
        /// in both — which is the difference between travelling and accumulating a blend map per
        /// chunk walked past.
        TEST(RtxSceneDescTest, releasingAMaterialGivesBackItsLayersAndMasks)
        {
            SceneDesc scene;
            const Index ground = scene.addTexture(VFS::Path::NormalizedView("textures/tx_ground.dds"));
            const Index stone = scene.addTexture(VFS::Path::NormalizedView("textures/tx_stone.dds"));
            const Index sand = scene.addTexture(VFS::Path::NormalizedView("textures/tx_sand.dds"));
            const Index moss = scene.addTexture(VFS::Path::NormalizedView("textures/tx_moss.dds"));
            ASSERT_EQ(moss, 3u);

            const std::array sGroundWeights{ 0.25f, 0.25f, 0.25f, 0.25f };
            const std::array sSandWeights{ 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };

            const std::array droppedLayers{ MaterialLayer{
                .mDiffuse = ground, .mMaskOffset = scene.addMask(sGroundWeights), .mMaskWidth = 2, .mMaskHeight = 2 } };
            const Span droppedRun = scene.addLayers(droppedLayers);
            const Index dropped = scene.addMaterial(Material{
                .mKind = MaterialKind::Terrain, .mLayerOffset = droppedRun.mOffset, .mLayerCount = droppedRun.mCount });

            const Index plain = scene.addMaterial(Material{ .mDiffuse = stone });

            const std::array keptLayers{
                MaterialLayer{
                    .mDiffuse = sand, .mMaskOffset = scene.addMask(sSandWeights), .mMaskWidth = 3, .mMaskHeight = 3 },
                MaterialLayer{ .mDiffuse = moss }
            };
            const Span keptRun = scene.addLayers(keptLayers);
            const Index kept = scene.addMaterial(Material{
                .mKind = MaterialKind::Terrain, .mLayerOffset = keptRun.mOffset, .mLayerCount = keptRun.mCount });

            ASSERT_EQ(scene.getLayers().size(), 3u);
            ASSERT_EQ(scene.getMasks().size(), 13u);

            const std::array<Index, 0> noMeshes{};
            const std::array materials{ plain, kept };
            ASSERT_TRUE(scene.release(noMeshes, materials));

            // One texture went with the material that wore it, and it stopped being an arrival.
            EXPECT_EQ(sorted(scene.getFreedTextures()), (std::vector<Index>{ ground }));
            EXPECT_EQ(sorted(scene.getArrivedTextures()), (std::vector<Index>{ stone, sand, moss }));

            // Every survivor is at the index it was given, which is what nothing moving means.
            EXPECT_EQ(scene.getMaterials().size(), 3u);
            EXPECT_EQ(scene.getMaterials()[plain].mDiffuse, stone);
            EXPECT_EQ(scene.getMaterials()[kept].mLayerOffset, 1u);
            EXPECT_EQ(scene.getMaterials()[kept].mLayerCount, 2u);

            EXPECT_EQ(scene.getLayers().size(), 3u) << "the tables never shrink, they are reused in place";
            EXPECT_EQ(scene.getMasks().size(), 13u);
            EXPECT_EQ(scene.getLayers()[1].mDiffuse, sand);
            EXPECT_EQ(scene.getLayers()[2].mDiffuse, moss);

            // **The next chunk of the same shape lands in the hole the first one left.** One layer
            // and four weights, which is exactly what went: both come back at zero and neither table
            // is any longer than it was.
            const std::array arrivingLayers{ MaterialLayer{
                .mDiffuse = moss, .mMaskOffset = scene.addMask(sGroundWeights), .mMaskWidth = 2, .mMaskHeight = 2 } };
            const Span arrivingRun = scene.addLayers(arrivingLayers);

            EXPECT_EQ(arrivingLayers[0].mMaskOffset, 0u) << "the freed mask run";
            EXPECT_EQ(arrivingRun, (Span{ .mOffset = 0, .mCount = 1 })) << "the freed layer run";
            EXPECT_EQ(scene.getLayers().size(), 3u) << "the layer table grew past a hole that fitted";
            EXPECT_EQ(scene.getMasks().size(), 13u) << "the mask table grew past a hole that fitted";

            // The freed slot goes to the next material asked for, whatever size it is: a material is
            // one size, so there is no fit to find.
            const Index reused = scene.addMaterial(Material{ .mDiffuse = moss });
            EXPECT_EQ(reused, dropped);
            EXPECT_EQ(scene.getMaterials().size(), 3u);

            // **`tx_ground` goes with the layer that named it, and its slot comes back.** Only the
            // dead material's run wore it, and an orphaned run is deliberately not allowed to speak
            // for a texture — or the image would leak alongside the layers.
            ASSERT_EQ(scene.getTextures().size(), 4u) << "the table shrank, so something was renumbered";
            EXPECT_TRUE(scene.getTextures()[ground].value().empty()) << "a texture nothing wears was kept";

            // The three the survivors wear are untouched, at the indices they were given.
            EXPECT_EQ(scene.getTextures()[stone], VFS::Path::NormalizedView("textures/tx_stone.dds"));
            EXPECT_EQ(scene.getTextures()[sand], VFS::Path::NormalizedView("textures/tx_sand.dds"));
            EXPECT_EQ(scene.getTextures()[moss], VFS::Path::NormalizedView("textures/tx_moss.dds"));

            // The freed slot is what the next texture takes, and the path lookup went with it: asking
            // for `tx_ground` again is a new arrival rather than a hit on a slot nothing stands in.
            EXPECT_EQ(scene.addTexture(VFS::Path::NormalizedView("textures/tx_ground.dds")), ground);
            EXPECT_EQ(scene.getTextures().size(), 4u) << "the table grew past a free slot";
            EXPECT_EQ(scene.getArrivedTextures().back(), ground) << "a slot taken over was not reported as arriving";
            EXPECT_TRUE(scene.getFreedTextures().empty()) << "a slot taken back was still reported as gone";
        }

        /// **The split that keeps an animated state set from rebuilding the world.**
        ///
        /// A material appearing, and a sweep that takes one away again, is a few kilobytes of table.
        /// A mesh or a texture *appearing* is every acceleration structure in the scene. The mirror
        /// reports them apart so a reader can answer them apart — OpenMW's water cycles thirty-two
        /// materials a second, and reading that as a world arriving cost the game every frame it
        /// had.
        TEST(RtxSceneDescTest, aMaterialChangingIsNotAStructureChanging)
        {
            SceneDesc scene;
            const Index mesh = scene.addMesh(sQuadPositions, {}, {}, sQuadIndices);
            scene.addMaterial(Material{});

            const std::uint64_t structure = scene.getStructureRevision();
            const std::uint64_t shading = scene.getShadingRevision();

            // A second material, which is what a state set with a new address comes to.
            Material other;
            other.mTwoSided = true;
            const Index kept = scene.addMaterial(other);

            EXPECT_EQ(scene.getStructureRevision(), structure) << "a material asked for a rebuild";
            EXPECT_GT(scene.getShadingRevision(), shading);

            // And taking one away again is the same kind of change, not a different one.
            const std::uint64_t settled = scene.getShadingRevision();
            const std::array meshes{ mesh };
            const std::array materials{ kept };

            ASSERT_TRUE(scene.release(meshes, materials));
            EXPECT_EQ(scene.getStructureRevision(), structure) << "a sweep of one material asked for a rebuild";
            EXPECT_GT(scene.getShadingRevision(), settled);

            // **And a mesh going is no longer the other answer either.** It was, while a sweep
            // compacted: the table moved and everything built from it had to be built again. A slot
            // that is freed in place invalidates nothing, so the frame after a cell leaves costs the
            // top level and nothing else.
            const std::uint64_t before = scene.getStructureRevision();
            ASSERT_TRUE(scene.release({}, materials));
            EXPECT_EQ(scene.getStructureRevision(), before) << "a cell leaving asked for a rebuild";
        }

        /// A scene that lost nothing is left entirely alone, and a sprite's texture is the caller's
        /// to speak for.
        TEST(RtxSceneDescTest, releasingDoesNothingWhenNothingWent)
        {
            SceneDesc scene;
            const Index mesh = scene.addMesh(sQuadPositions, {}, {}, sQuadIndices);
            const Index material = scene.addMaterial(Material{});
            scene.addTexture(VFS::Path::NormalizedView("textures/tx_fire_00.dds"));

            const std::array meshes{ mesh };
            const std::array materials{ material };
            const std::uint64_t was = scene.getStructureRevision();
            const std::uint64_t shading = scene.getShadingRevision();

            EXPECT_FALSE(scene.release(meshes, materials));
            EXPECT_EQ(scene.getStructureRevision(), was);
            EXPECT_EQ(scene.getShadingRevision(), shading);
            EXPECT_TRUE(scene.getFreedMeshes().empty()) << "a sweep that freed nothing named something";
            EXPECT_TRUE(scene.getFreedTextures().empty());

            // Asked again with everything already free, which is the frame after a cell left: the
            // live count is what the keep set is compared against, not the table's size.
            const std::array<Index, 0> none{};
            ASSERT_TRUE(scene.release(none, none));
            EXPECT_EQ(sorted(scene.getFreedMeshes()), (std::vector<Index>{ mesh }));

            EXPECT_FALSE(scene.release(none, none)) << "a table with nothing left in it went again";
            EXPECT_EQ(sorted(scene.getFreedMeshes()), (std::vector<Index>{ mesh })) << "a slot went twice";

            // A texture nothing has been told to name is nobody's to give back, so it stays — which
            // is what `addTexture` says of a caller that asks for one and then puts it nowhere.
            EXPECT_EQ(scene.getTextures().size(), 1u);
            EXPECT_TRUE(scene.getFreedTextures().empty());
        }

        /// A sweep leaves the per-frame lists as the walk left them, because the frame it happens on
        /// is about to be drawn from them. Emptying them here left every lamp in the world dark for
        /// exactly one frame, on the frames a sweep freed something.
        TEST(RtxSceneDescTest, aSweepLeavesTheListsTheWalkFilled)
        {
            SceneDesc scene;
            const Index kept = scene.addMesh(sQuadPositions, {}, {}, sQuadIndices);
            scene.addMesh(sQuadPositions, {}, {}, sQuadIndices);

            scene.addLight(Light{ .mPosition = osg::Vec3f(1.0f, 2.0f, 3.0f),
                .mIntensity = osg::Vec3f(4.0f, 5.0f, 6.0f),
                .mReach = 256.0f });
            scene.addLight(Light{ .mPosition = osg::Vec3f(-7.0f, 8.0f, 9.0f),
                .mIntensity = osg::Vec3f(1.0f, 1.0f, 1.0f),
                .mReach = 512.0f });

            const std::array meshes{ kept };
            const std::array<Index, 0> noMaterials{};
            ASSERT_TRUE(scene.release(meshes, noMaterials)) << "the second mesh should have gone";

            ASSERT_EQ(scene.getLights().size(), 2u) << "the sweep emptied the light table the walk had just filled";
            EXPECT_EQ(scene.getLights()[0].mPosition, osg::Vec3f(1.0f, 2.0f, 3.0f));
            EXPECT_EQ(scene.getLights()[0].mReach, 256.0f);
            EXPECT_EQ(scene.getLights()[1].mPosition, osg::Vec3f(-7.0f, 8.0f, 9.0f));
            EXPECT_EQ(scene.getLights()[1].mReach, 512.0f);

            // Emptying them is still `clearPlacement`'s, which is what the next walk begins with.
            scene.clearPlacement();
            EXPECT_TRUE(scene.getLights().empty());
        }

        /// A texture goes with the last material that names it, and not with the first.
        ///
        /// **The case a sweep could only answer on some frames.** Freeing used to be a walk of the
        /// live materials run from `release`, and `release` returns before it starts whenever the
        /// mesh and material counts say nothing died. Counting the names instead makes the answer
        /// the same whatever else the frame did.
        TEST(RtxSceneDescTest, aTextureGoesWithTheLastMaterialThatNamesIt)
        {
            SceneDesc scene;
            const Index mesh = scene.addMesh(sQuadPositions, {}, {}, sQuadIndices);
            const Index shared = scene.addTexture(VFS::Path::NormalizedView("textures/tx_stone.dds"));
            const Index lone = scene.addTexture(VFS::Path::NormalizedView("textures/tx_sand.dds"));

            scene.addMaterial(Material{ .mDiffuse = shared });
            const Index second = scene.addMaterial(Material{ .mDiffuse = shared, .mNormal = lone });

            const std::array meshes{ mesh };
            const std::array keepSecond{ second };
            ASSERT_TRUE(scene.release(meshes, keepSecond));

            EXPECT_TRUE(scene.getFreedTextures().empty()) << "a texture another material still names";
            EXPECT_EQ(scene.getTextures()[shared], VFS::Path::NormalizedView("textures/tx_stone.dds"));

            const std::array<Index, 0> none{};
            ASSERT_TRUE(scene.release(meshes, none));

            EXPECT_EQ(sorted(scene.getFreedTextures()), (std::vector<Index>{ shared, lone }));
            EXPECT_TRUE(scene.getTextures()[shared].value().empty());
            EXPECT_TRUE(scene.getTextures()[lone].value().empty());
        }

        /// An image with no file behind it takes a slot like any other and gives it back like any
        /// other.
        ///
        /// **What a composite baked for a distant terrain chunk is.** Nothing can open it — the bytes
        /// belong to whatever made it — but it is still a slot a material points at and a backend
        /// uploads into, so it has to live in the one table, on the one free list, under the one
        /// reference count. A second table would be a second lifetime for a thing that dies the same
        /// way.
        TEST(RtxSceneDescTest, anImageThatIsNotAFileTakesASlotAndGivesItBack)
        {
            SceneDesc scene;

            const Index baked = scene.addBakedTexture("composite/-3,-2/2");
            ASSERT_EQ(baked, 0u);

            // Standing, and standing is not free — the path is empty because it has none, which is
            // the same thing a free slot's path says and not the same fact.
            EXPECT_FALSE(scene.isTextureFree(baked));
            EXPECT_TRUE(scene.getTextures()[baked].value().empty()) << "it came from no file";
            EXPECT_EQ(scene.getBakedTextures()[baked], "composite/-3,-2/2");

            // The key is what makes two chunks that would bake the same image share one slot.
            EXPECT_EQ(scene.addBakedTexture("composite/-3,-2/2"), baked) << "the same bake took a second slot";
            EXPECT_EQ(scene.getTextures().size(), 1u);

            // A file beside it, so the free list has to hand back the right one.
            const Index file = scene.addTexture(VFS::Path::NormalizedView("textures/tx_stone.dds"));
            ASSERT_EQ(file, 1u);

            scene.holdTexture(baked);
            scene.dropTexture(baked);

            EXPECT_TRUE(scene.isTextureFree(baked)) << "nothing names it and it is still standing";
            EXPECT_TRUE(scene.getBakedTextures()[baked].empty());
            EXPECT_EQ(sorted(scene.getFreedTextures()), (std::vector<Index>{ baked }));

            // And the slot comes back, to a file this time — a freed slot is a row and not a kind.
            const Index next = scene.addTexture(VFS::Path::NormalizedView("textures/tx_sand.dds"));
            EXPECT_EQ(next, baked) << "the table grew past a free slot";
            EXPECT_EQ(scene.getTextures().size(), 2u);
            EXPECT_EQ(scene.getTextures()[next], VFS::Path::NormalizedView("textures/tx_sand.dds"));
            EXPECT_TRUE(scene.getBakedTextures()[next].empty()) << "the slot kept what the last tenant was";

            // The key is free again too, or a bake that came back would find a slot somebody else has.
            const Index again = scene.addBakedTexture("composite/-3,-2/2");
            EXPECT_EQ(again, 2u) << "a key the table gave back found a slot somebody else has";
            EXPECT_FALSE(scene.isTextureFree(file)) << "the file beside it was never touched";
        }

        /// A material rewritten gives back what it stopped naming and keeps what it still names.
        ///
        /// **What a flipbook is**: `NifOsg` turns a fire over thirty-two times a second by rewriting
        /// one state set, and the surface wearing it never moves. The material keeps its slot; the
        /// image it walked away from does not.
        TEST(RtxSceneDescTest, aMaterialRewrittenGivesBackOnlyWhatItStoppedNaming)
        {
            SceneDesc scene;
            const Index first = scene.addTexture(VFS::Path::NormalizedView("textures/tx_fire_00.dds"));
            const Index second = scene.addTexture(VFS::Path::NormalizedView("textures/tx_fire_01.dds"));
            const Index material = scene.addMaterial(Material{ .mDiffuse = first });

            scene.setMaterial(material, Material{ .mDiffuse = second });

            EXPECT_EQ(sorted(scene.getFreedTextures()), (std::vector<Index>{ first }));
            EXPECT_TRUE(scene.getTextures()[first].value().empty()) << "the frame it left is still named";
            EXPECT_EQ(scene.getTextures()[second], VFS::Path::NormalizedView("textures/tx_fire_01.dds"));

            // **And round again onto a frame it already had.** Taking the new set before giving the
            // old one back is the whole of what stops this: the other order takes the slot to zero,
            // empties its path and hands it to the next thing that asks for one — a texture changing
            // identity under a material that never stopped naming it.
            scene.setMaterial(material, Material{ .mDiffuse = second, .mTwoSided = true });

            EXPECT_EQ(scene.getTextures()[second], VFS::Path::NormalizedView("textures/tx_fire_01.dds"))
                << "a texture the material still names was let go and taken again";
            EXPECT_EQ(sorted(scene.getFreedTextures()), (std::vector<Index>{ first })) << "and reported as going";
        }

        /// A hold speaks for a texture no material can, and the slot goes when the hold does.
        TEST(RtxSceneDescTest, aHeldTextureGoesWhenTheHoldDoesAndNotBefore)
        {
            SceneDesc scene;
            const Index mesh = scene.addMesh(sQuadPositions, {}, {}, sQuadIndices);
            const Index material = scene.addMaterial(Material{});
            const Index sprite = scene.addTexture(VFS::Path::NormalizedView("textures/tx_fire_00.dds"));
            scene.holdTexture(sprite);

            // The ordinary frame, where the sweep answers with two comparisons and returns.
            const std::array meshes{ mesh };
            const std::array materials{ material };
            EXPECT_FALSE(scene.release(meshes, materials));
            EXPECT_EQ(scene.getTextures()[sprite], VFS::Path::NormalizedView("textures/tx_fire_00.dds"));

            scene.dropTexture(sprite);

            EXPECT_EQ(sorted(scene.getFreedTextures()), (std::vector<Index>{ sprite }));
            EXPECT_TRUE(scene.getTextures()[sprite].value().empty());

            // And the slot is handed out again rather than the table growing.
            EXPECT_EQ(scene.addTexture(VFS::Path::NormalizedView("textures/tx_smoke.dds")), sprite);
            EXPECT_EQ(scene.getTextures().size(), 1u);
        }

        /// A mesh's vertices never straddle a block, and the tail one skipped is handed out again.
        ///
        /// **What lets the device hold a list of buffers rather than one.** A buffer that is a single
        /// allocation moves when it grows, and every bottom-level acceleration structure holds a
        /// device address into it; blocked, each block is allocated once and never moves. The rule
        /// that buys that is the one asserted here — a run lies inside one block or it is not placed
        /// there — and the price is the tail, which must go back into circulation or a scene would
        /// leak most of a block per boundary crossed.
        ///
        /// Hand-computed against a block of 262,144. Two hundred thousand vertices leave 62,144 of
        /// the first block; a hundred thousand cannot fit in that, so it starts the second and the
        /// tail stays behind; sixty thousand then fits the tail and takes it at 200,000.
        TEST(RtxSceneDescTest, aMeshNeverStraddlesABlockAndTheTailItSkippedIsReused)
        {
            ASSERT_EQ(SceneDesc::sVertexBlock, 262144u) << "the arithmetic below is written against this";

            // One buffer, sliced. A block is a quarter of a million vertices and three separate
            // copies of that is memory this test has no use for.
            const std::vector<osg::Vec3f> room(SceneDesc::sVertexBlock);
            const std::array<std::uint32_t, 3> triangle{ 0, 1, 2 };

            const auto vertices = [&](std::size_t count) { return std::span(room).first(count); };

            SceneDesc scene;
            const Index first = scene.addMesh(vertices(200000), {}, {}, triangle);
            EXPECT_EQ(scene.getMeshes()[first].mVertexOffset, 0u);

            const Index second = scene.addMesh(vertices(100000), {}, {}, triangle);
            EXPECT_EQ(scene.getMeshes()[second].mVertexOffset, SceneDesc::sVertexBlock)
                << "a run was laid across a block boundary";
            EXPECT_EQ(scene.getPositions().size(), std::size_t{ 362144 });

            // And the 62,144 the second one stepped over is a hole like any other.
            const Index third = scene.addMesh(vertices(60000), {}, {}, triangle);
            EXPECT_EQ(scene.getMeshes()[third].mVertexOffset, 200000u) << "the tail of a block was not reused";
            EXPECT_EQ(scene.getPositions().size(), std::size_t{ 362144 }) << "a mesh that fitted the tail appended";

            // None of the three crosses a boundary, which is the property rather than the three
            // offsets that happen to demonstrate it.
            for (const Index mesh : { first, second, third })
            {
                const MeshRange& range = scene.getMeshes()[mesh];
                EXPECT_EQ(range.mVertexOffset / SceneDesc::sVertexBlock,
                    (range.mVertexOffset + range.mVertexCount - 1) / SceneDesc::sVertexBlock)
                    << "mesh " << mesh << " straddles a block";
            }
        }

        /// A mesh longer than a block is refused by name rather than written across two of them.
        ///
        /// **Not an assert, because a vertex count comes out of a content file.** A run that
        /// straddled a block would be written across two device allocations that are not next to
        /// each other, which is not a wrong picture but a wild write.
        TEST(RtxSceneDescTest, aMeshLongerThanABlockIsRefusedByName)
        {
            const std::vector<osg::Vec3f> tooMany(SceneDesc::sVertexBlock + 1);
            const std::array<std::uint32_t, 3> triangle{ 0, 1, 2 };

            SceneDesc scene;
            EXPECT_THROW(scene.addMesh(tooMany, {}, {}, triangle), Error);

            // And exactly a block is not too many, so the refusal is a boundary and not a ban.
            EXPECT_NO_THROW(scene.addMesh(std::span(tooMany).first(SceneDesc::sVertexBlock), {}, {}, triangle));
        }

        TEST(RtxSceneDescTest, clearingEmptiesEveryTable)
        {
            SceneDesc scene;
            const Index mesh = scene.addMesh(sQuadPositions, {}, {}, sQuadIndices);
            const Index material = scene.addMaterial(Material{});
            scene.addTexture(VFS::Path::NormalizedView("textures/tx_stone_01.dds"));
            scene.addBakedTexture("composite/0,0/1");
            scene.addInstance(
                MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = mesh, .mMaterial = material });
            scene.updateMesh(mesh, sQuadPositions, {});
            scene.addEmitter(std::array{ Sprite{ .mRadius = 1.0f } }, 0, true);

            scene.clear();

            EXPECT_TRUE(scene.getMeshes().empty());
            EXPECT_TRUE(scene.getInstances().empty());
            EXPECT_TRUE(scene.getMaterials().empty());
            EXPECT_TRUE(scene.getTextures().empty());
            EXPECT_TRUE(scene.getBakedTextures().empty());
            EXPECT_TRUE(scene.getPositions().empty());
            EXPECT_TRUE(scene.getDeformed().empty());
            EXPECT_TRUE(scene.getSprites().empty());
            EXPECT_TRUE(scene.getEmitters().empty());
            EXPECT_EQ(scene.getTriangleCount(), 0u);

            // A reset renumbers, so nothing that arrived or went under the old numbering means
            // anything: a backend hearing this rebuilds rather than applying either list.
            EXPECT_TRUE(scene.getArrivedMeshes().empty());
            EXPECT_TRUE(scene.getFreedMeshes().empty());
            EXPECT_TRUE(scene.getArrivedTextures().empty());
            EXPECT_TRUE(scene.getFreedTextures().empty());

            // And the lookups with them, or a key from the world before this one finds a slot in the
            // world after it.
            EXPECT_EQ(scene.addBakedTexture("composite/0,0/1"), 0u);
        }

        /// A camera is placed from what stands in a region, and the sea is not among it.
        ///
        /// **Two failures, one call.** The sea is one sheet a hundred and fifty cells across, laid
        /// down by the world rather than by any cell, so framing everything placed put the eye a
        /// million and a half units from a village. And the ground now reaches four cells past the
        /// one being looked at, so framing everything that is not the sea still framed a region. A
        /// view names a place; this is the extent of that place.
        TEST(RtxSceneDescTest, aRegionsExtentLeavesOutTheSeaAndStopsAtItsOwnEdge)
        {
            SceneDesc scene;

            const Index quad = scene.addMesh(sQuadPositions, {}, {}, sQuadIndices);
            const Index ground = scene.addMaterial(Material{ .mKind = MaterialKind::Terrain });
            const Index sea = scene.addMaterial(Material{ .mKind = MaterialKind::Water });

            // One unit square at the origin, and a sheet ten thousand across under everything.
            scene.addInstance(
                MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = quad, .mMaterial = ground });
            scene.addInstance(MeshInstance{
                .mTransform = osg::Matrixf::scale(10000.0f, 10000.0f, 1.0f), .mMesh = quad, .mMaterial = sea });

            // Everything, which is what a far plane asks for and why the sea is still in the table.
            EXPECT_FLOAT_EQ(scene.getBounds().xMax(), 10000.0f);

            const osg::BoundingBoxf everywhere(-1e9f, -1e9f, -1e9f, 1e9f, 1e9f, 1e9f);
            const osg::BoundingBoxf content = scene.getContentBoundsWithin(everywhere);

            ASSERT_TRUE(content.valid());
            EXPECT_FLOAT_EQ(content.xMax(), 1.0f) << "the sea was framed";
            EXPECT_FLOAT_EQ(content.yMax(), 1.0f);

            // **And the region clips.** A chunk straddling the edge contributes where it overlaps
            // rather than dragging the answer out by its whole width, which is what keeps a view of
            // one cell from framing the four cells of ground that reach into it.
            const Index wide = scene.addMesh(sQuadPositions, {}, {}, sQuadIndices);
            scene.addInstance(MeshInstance{
                .mTransform = osg::Matrixf::scale(100.0f, 1.0f, 1.0f), .mMesh = wide, .mMaterial = ground });

            const osg::BoundingBoxf narrow(-1.0f, -1.0f, -1.0f, 4.0f, 4.0f, 4.0f);
            const osg::BoundingBoxf clipped = scene.getContentBoundsWithin(narrow);

            ASSERT_TRUE(clipped.valid());
            EXPECT_FLOAT_EQ(clipped.xMax(), 4.0f) << "a chunk reaching past the edge widened the region";

            // Nothing stands out there, and an empty answer is what says so rather than a box at the
            // origin that a camera would then be placed from.
            EXPECT_FALSE(scene.getContentBoundsWithin(osg::BoundingBoxf(500.0f, 500.0f, 500.0f, 600.0f, 600.0f, 600.0f))
                             .valid());
        }

    }
}