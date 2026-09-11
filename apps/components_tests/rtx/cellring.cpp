#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Geometry>
#include <osg/Group>
#include <osg/Image>
#include <osg/Matrix>
#include <osg/MatrixTransform>
#include <osg/Matrixf>
#include <osg/Quat>
#include <osg/Vec2i>
#include <osg/Vec3f>
#include <osg/Vec4i>

#include <components/esm/defs.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm3/refnum.hpp>
#include <components/misc/constants.hpp>
#include <components/rtx/cellring.hpp>
#include <components/rtx/contentsource.hpp>
#include <components/rtx/extractionstats.hpp>
#include <components/rtx/material.hpp>
#include <components/rtx/meshinstance.hpp>
#include <components/rtx/preparedtexture.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/terrain/objectstorage.hpp>
#include <components/vfs/pathutil.hpp>

#include "allocations.hpp"
#include "extractor/fixture.hpp"
#include "fakeland.hpp"
#include "geometry.hpp"

namespace Rtx::Testing
{
    namespace
    {
        constexpr float sCellSize = static_cast<float>(Constants::CellSizeInUnits);

        /// One reference a storage stands.
        struct Placed
        {
            osg::Vec2i mCell;
            const char* mModel = nullptr;
            ESM::RefNum mRefNum;
            osg::Vec3f mPosition;
            osg::Vec3f mRotation;
            float mScale = 1.0f;
        };

        /// A storage of a handful of statics, each in a cell of its own choosing.
        class FewStatics final : public Terrain::ObjectStorage
        {
        public:
            std::vector<Placed> mPlaced;

            void collect(Terrain::RefKind kind, float, const osg::Vec2i& startCell, ESM::RefId,
                std::vector<Terrain::PagedCellRef>& out) const override
            {
                out.clear();
                if (kind != Terrain::RefKind::Paged)
                    return;

                for (const Placed& placed : mPlaced)
                    if (placed.mCell == startCell)
                        out.push_back(Terrain::PagedCellRef{
                            .mRefId = ESM::RefId::stringRefId(placed.mModel),
                            .mRefNum = placed.mRefNum,
                            .mPosition = placed.mPosition,
                            .mRotation = placed.mRotation,
                            .mScale = placed.mScale,
                            .mType = ESM::REC_STAT,
                        });
            }

            std::optional<SceneUtil::LightCommon> getLight(const ESM::RefId&) const override { return std::nullopt; }

            /// The record's id doubles as its model here.
            VFS::Path::Normalized getModel(int, const ESM::RefId& id) const override
            {
                return VFS::Path::Normalized(id.getRefIdString());
            }

            int getEsmVersion(int) const override { return 0; }
        };

        /// Templates by name — a square sheet of a radius the size rule can be asked about, five
        /// units up under a transform of its own — and an image for every path the ground asks.
        class FewContent final : public ContentSource
        {
        public:
            /// By the corrected path, which is what a reader asks for: `correctMeshPath` puts
            /// every model under `meshes/`.
            osg::ref_ptr<const osg::Node> getTemplate(VFS::Path::NormalizedView path) override
            {
                if (path.value() == "meshes/tree.nif")
                    return mTree;
                if (path.value() == "meshes/fern.nif")
                    return mFern;
                return nullptr;
            }

            osg::ref_ptr<const osg::Image> getImage(const VFS::Path::NormalizedView path) override
            {
                return mImages.get(path);
            }

            ImagesByPath mImages;

            /// A four-by-four grey with one level, which is an image the ring has a chain to build
            /// for and a shading to estimate.
            osg::ref_ptr<osg::Image> mBark = [] {
                osg::ref_ptr<osg::Image> image = new osg::Image;
                image->setFileName("textures/bark.dds");
                image->allocateImage(4, 4, 1, GL_RGBA, GL_UNSIGNED_BYTE);
                std::fill_n(image->data(), image->getTotalSizeInBytes(), static_cast<unsigned char>(128));
                return image;
            }();

            osg::ref_ptr<osg::Group> mTree = makeSheet(300.0f);
            osg::ref_ptr<osg::Group> mFern = makeSheet(20.0f);

        private:
            osg::ref_ptr<osg::Group> makeSheet(float extent) const
            {
                osg::ref_ptr<osg::Geometry> sheet = new osg::Geometry;
                const auto corners = sheetAt(extent, 0.0f);
                sheet->setVertexArray(makePositions({ corners[0], corners[1], corners[2], corners[3] }));
                sheet->addPrimitiveSet(makeTriangles({ 0, 1, 2, 0, 2, 3 }));
                paint(*sheet->getOrCreateStateSet(), *mBark);

                osg::ref_ptr<osg::MatrixTransform> lifted
                    = new osg::MatrixTransform(osg::Matrix::translate(0.0f, 0.0f, 5.0f));
                lifted->addChild(sheet);

                osg::ref_ptr<osg::Group> root = new osg::Group;
                root->addChild(lifted);
                return root;
            }
        };

        /// Where the game stands a clone of a reference: the transform `MWRender::Objects` builds,
        /// with the quaternion the paging and the objects both spell.
        osg::Matrixf gameStands(const Placed& placed)
        {
            osg::ref_ptr<SceneUtil::PositionAttitudeTransform> stand = new SceneUtil::PositionAttitudeTransform;
            stand->setPosition(placed.mPosition);
            stand->setAttitude(osg::Quat(placed.mRotation.z(), osg::Vec3f(0.0f, 0.0f, -1.0f))
                * osg::Quat(placed.mRotation.y(), osg::Vec3f(0.0f, -1.0f, 0.0f))
                * osg::Quat(placed.mRotation.x(), osg::Vec3f(-1.0f, 0.0f, 0.0f)));
            stand->setScale(osg::Vec3f(placed.mScale, placed.mScale, placed.mScale));

            osg::Matrix matrix;
            stand->computeLocalToWorldMatrix(matrix, nullptr);
            return osg::Matrixf(matrix);
        }

        /// The number of cells a band of `reach` cells about the eye holds.
        constexpr std::uint32_t cellsWithin(const int reach)
        {
            return static_cast<std::uint32_t>((2 * reach + 1) * (2 * reach + 1));
        }

        /// The placed ring at a reach of four cells, and the prepared ring a band wider.
        constexpr std::uint32_t sPlacedCells = cellsWithin(4);
        constexpr std::uint32_t sPreparedCells = cellsWithin(5);

        /// A world with nothing on its graph and a ring beside it, walked from a fixed eye. Every
        /// cell of the land has a record, so every cell stands two ground types.
        class RtxCellRingTest : public ::testing::Test
        {
        protected:
            RtxCellRingTest()
            {
                for (int x = -8; x <= 30; ++x)
                    for (int y = -8; y <= 30; ++y)
                        mLand.mWithData.emplace_back(x, y);

                mExtractor.follow(std::array<Residency*, 1>{ &mRing });
                mRing.setMinSize(0.0f);
                mRing.setSettled(true);

                mAround.mReach = 4.0f * sCellSize;
                mAround.mActiveGrid = osg::Vec4i(-1, -1, 2, 2);
                mAround.mEye = osg::Vec3f(0.5f * sCellSize, 0.5f * sCellSize, 0.0f);
                mAround.mOutdoors = true;
            }

            void start()
            {
                mAround.mWorld = Rtx::CellWorld{
                    .mStorage = &mStorage,
                    .mGround = &mLand,
                    .mContent = &mContent,
                    .mWorldspace = ESM::Cell::sDefaultWorldspaceId,
                    .mMask = ~0u,
                };
                mRing.follow(mAround);
            }

            /// Says where the eye stands and what the game holds around it, as one value.
            void around(const osg::Vec3f& eye, const osg::Vec4i& grid)
            {
                mAround.mEye = eye;
                mAround.mActiveGrid = grid;
                mRing.follow(mAround);
            }

            /// The same for a grid the game moved without the eye moving with it.
            void around(const osg::Vec4i& grid) { around(mAround.mEye, grid); }

            /// One frame's walk, which is where the ring adopts, places and stamps.
            ExtractionStats walk(std::size_t frame)
            {
                mRing.setFrame(frame);
                const ExtractionStats stats = mExtractor.extractWorld(*mEmpty, osg::Matrixf::identity(), 0, frame);
                mExtractor.advance();
                return stats;
            }

            /// Walks until the ring has adopted every cell the band wants, and answers what those
            /// walks came to.
            ///
            /// **A walk adopts one cell, settled or not**, so a band of `sPreparedCells` is that
            /// many walks. `CellRing::setSettled` says why the settled rule is the wait and not the
            /// count.
            ///
            /// What a walk added is summed, and what stands is the last walk's: a sum of the
            /// standing counts would count the whole ring once for every walk it took to build.
            ExtractionStats fill()
            {
                ExtractionStats total;
                ExtractionStats last;
                do
                {
                    last = walk(mWalked++);
                    total += last;
                } while (last.mMeshesAdded > 0);

                total.mDistantStatics = last.mDistantStatics;
                total.mGroundCells = last.mGroundCells;
                total.mInstances = last.mInstances;
                return total;
            }

            std::uint32_t placed() const { return mScene.getTables().mPlacements.getPlacedCount(); }

            /// The placement standing the ground of `cell`, which is the one translated to the
            /// cell's centre.
            std::optional<MeshInstance> groundOf(const osg::Vec2i& cell) const
            {
                const osg::Vec3f centre((static_cast<float>(cell.x()) + 0.5f) * sCellSize,
                    (static_cast<float>(cell.y()) + 0.5f) * sCellSize, 0.0f);

                for (const MeshInstance& placement : mScene.getTables().mPlacements.getAll())
                    if (placement.isPlaced() && placement.mTransform.getTrans() == centre)
                        return placement;

                return std::nullopt;
            }

            /// The frame the next walk is for, so every walk of a test is a frame of its own.
            std::size_t mWalked = 1;

            WorldAround mAround;

            FewStatics mStorage;
            FakeLand mLand;
            FewContent mContent;
            osg::ref_ptr<osg::Group> mEmpty = new osg::Group;

            SceneDesc mScene;
            SceneExtractor mExtractor{ mScene };
            CellRing mRing{ mScene };
        };

        /// A reference stands where the game would stand its clone, on the mesh every copy shares;
        /// what the active grid holds is left to the game; every cell of the reach stands its
        /// ground on a row of the ring's own; and a second walk adds nothing.
        TEST_F(RtxCellRingTest, referencesStandWhereTheGameWouldStandThemOnOneMeshEach)
        {
            const Placed tree{ .mCell = osg::Vec2i(3, 0),
                .mModel = "tree.nif",
                .mRefNum = ESM::RefNum{ 1, 0 },
                .mPosition = osg::Vec3f(3.5f * sCellSize, 0.25f * sCellSize, 100.0f),
                .mRotation = osg::Vec3f(0.1f, 0.2f, 1.5f),
                .mScale = 2.0f };
            const Placed anotherTree{ .mCell = osg::Vec2i(3, 0),
                .mModel = "tree.nif",
                .mRefNum = ESM::RefNum{ 2, 0 },
                .mPosition = osg::Vec3f(3.2f * sCellSize, 0.75f * sCellSize, 50.0f) };
            const Placed fern{ .mCell = osg::Vec2i(0, 3),
                .mModel = "fern.nif",
                .mRefNum = ESM::RefNum{ 3, 0 },
                .mPosition = osg::Vec3f(0.5f * sCellSize, 3.5f * sCellSize, 0.0f) };
            const Placed atHome{ .mCell = osg::Vec2i(0, 0),
                .mModel = "tree.nif",
                .mRefNum = ESM::RefNum{ 4, 0 },
                .mPosition = osg::Vec3f(0.5f * sCellSize, 0.5f * sCellSize, 0.0f) };
            const Placed beyond{ .mCell = osg::Vec2i(7, 0),
                .mModel = "tree.nif",
                .mRefNum = ESM::RefNum{ 5, 0 },
                .mPosition = osg::Vec3f(7.5f * sCellSize, 0.5f * sCellSize, 0.0f) };
            mStorage.mPlaced = { tree, anotherTree, fern, atHome, beyond };

            start();

            const ExtractionStats first = fill();
            EXPECT_EQ(first.mDistantStatics, 3u)
                << "two trees and a fern; the active grid's is the game's and the far one is past the reach";
            EXPECT_EQ(first.mGroundCells, sPlacedCells) << "nine by nine cells of ground, the active grid's included";
            EXPECT_EQ(first.mInstances, 3u + sPlacedCells);
            EXPECT_EQ(placed(), 3u + sPlacedCells);
            EXPECT_EQ(first.mMeshesAdded, 2u + sPreparedCells)
                << "one mesh for the tree however many stand, one for the fern, and one a cell of ground";
            EXPECT_EQ(first.mMaterialsAdded, 2u + sPreparedCells);
            EXPECT_EQ(mRing.getHeldCellCount(), sPreparedCells);

            // **The ground stands where the storage put it**: cell (3, 0)'s placement is translated
            // to the cell's centre, and its mesh's first vertex is the storage's own south-western
            // corner. Its two layers, outside the active grid, ask for a composite; the eye's own
            // cell shades from its stack.
            const std::optional<MeshInstance> far = groundOf(osg::Vec2i(3, 0));
            ASSERT_TRUE(far.has_value());
            EXPECT_EQ(mScene.getTables().mMeshes.getMeshPositions(far->mMesh)[0], FakeLand::positionAt(0, 0));
            EXPECT_EQ(mScene.getTables().mMeshes.getMeshPositions(far->mMesh).size(),
                static_cast<std::size_t>(FakeLand::sVerts) * FakeLand::sVerts);
            {
                const Material& material = mScene.getTables().mMaterials.getRows()[far->mMaterial];
                EXPECT_EQ(material.mKind, MaterialKind::Terrain);
                EXPECT_EQ(material.mLayers.mCount, 2u);
                EXPECT_TRUE(material.mFlatten);
            }
            const std::optional<MeshInstance> home = groundOf(osg::Vec2i(0, 0));
            ASSERT_TRUE(home.has_value());
            EXPECT_FALSE(mScene.getTables().mMaterials.getRows()[home->mMaterial].mFlatten);
            EXPECT_FALSE(groundOf(osg::Vec2i(5, 0)).has_value()) << "prepared a band out, and not placed";

            // The ground's textures were read on the thread as the bark was.
            EXPECT_NE(mRing.find(*mContent.mImages.get(VFS::Path::NormalizedView("textures/grass.dds"))), nullptr);

            // The bark was read on the thread: its chain built down from four to one, and its
            // shading estimated — a flat grey to one everywhere.
            const PreparedTexture* bark = mRing.find(*mContent.mBark);
            ASSERT_NE(bark, nullptr);
            EXPECT_TRUE(bark->mReadable);
            EXPECT_FALSE(bark->mChain.isEmpty());
            EXPECT_EQ(bark->mChain.describe().mLevels.size(), 3u);
            EXPECT_NEAR(bark->mShading[0], 1.0f, 0.01f);

            // The sheet's origin, lifted five units in the template and then stood as the game
            // stands the reference — which one placement of the three lands on. The cells are
            // walked in their own order, so the tree's is not the first placement.
            const osg::Vec3f expected = osg::Vec3f(0.0f, 0.0f, 5.0f) * gameStands(tree);
            std::size_t standing = 0;
            for (const MeshInstance& placement : mScene.getTables().mPlacements.getAll())
            {
                const osg::Vec3f stood = osg::Vec3f() * placement.mTransform;
                if ((stood - expected).length() < 0.01f)
                    ++standing;
            }
            EXPECT_EQ(standing, 1u) << "one placement stands where the game would stand the scaled, turned tree";

            // The other tree is neither turned nor scaled, so its sheet stands exactly five over
            // the reference: 50 + 5, by hand.
            const osg::Vec3f untilted = osg::Vec3f(0.0f, 0.0f, 5.0f) * gameStands(anotherTree);
            EXPECT_NEAR(untilted.z(), 55.0f, 0.001f);
            EXPECT_NEAR(untilted.x(), 3.2f * sCellSize, 0.01f);
            standing = 0;
            for (const MeshInstance& placement : mScene.getTables().mPlacements.getAll())
                if ((osg::Vec3f() * placement.mTransform - untilted).length() < 0.01f)
                    ++standing;
            EXPECT_EQ(standing, 1u);

            // **A steady frame reaches the heap zero times**, which is the rule every loader here
            // keeps: what the ring holds is placed and counted out of buffers it already grew.
            const std::size_t before = Testing::getAllocationCount();
            const ExtractionStats again = walk(mWalked++);
            const std::size_t spent = Testing::getAllocationCount() - before;
            EXPECT_EQ(spent, 0u) << "a steady walk of the ring reached the heap " << spent << " times";

            EXPECT_EQ(again.mMeshesAdded, 0u) << "a second walk of one frame adds nothing";
            EXPECT_EQ(again.mMaterialsAdded, 0u);
            EXPECT_EQ(again.mMeshesReused, 0u) << "what the ring holds is held, and is not met again to be reused";
            EXPECT_EQ(placed(), 3u + sPlacedCells);

            EXPECT_TRUE(mExtractor.retire().empty()) << "everything the ring holds is held through the sweep";

            // A script disables one tree: the walk after it stands two. Swept between walks, as
            // every frame of the game is.
            mRing.setReferenceEnabled(ESM::RefNum{ 1, 0 }, false);
            walk(2);
            EXPECT_EQ(placed(), 2u + sPlacedCells);
            EXPECT_TRUE(mExtractor.retire().empty()) << "a reference kept out still stands on a mesh the ring holds";
            mRing.setReferenceEnabled(ESM::RefNum{ 1, 0 }, true);
            walk(3);
            EXPECT_EQ(placed(), 3u + sPlacedCells);
            EXPECT_TRUE(mExtractor.retire().empty());

            // The active grid moves over cell (3, 0): its ground shades from its stack from now
            // on, on the same row, the two trees inside the grid are the game's, and the tree at
            // the eye's own cell — outside the grid now — is the ring's.
            around(osg::Vec4i(2, -1, 5, 2));
            walk(mWalked++);
            EXPECT_EQ(placed(), 2u + sPlacedCells) << "the fern and the tree at home stand outside the grid";
            EXPECT_FALSE(mScene.getTables().mMaterials.getRows()[far->mMaterial].mFlatten);
            EXPECT_TRUE(mExtractor.retire().empty());

            // The eye leaves for a cell far away. **The band that left goes on the first walk after
            // the move and the band that arrives comes a cell a walk after it**, so the sweep that
            // follows that one walk is where the meshes nothing stands on go — the two models' and
            // the ground of every cell that left.
            around(osg::Vec3f(20.5f * sCellSize, 20.5f * sCellSize, 0.0f), osg::Vec4i(19, 19, 22, 22));
            walk(mWalked++);

            const Retirement went = mExtractor.retire();
            EXPECT_EQ(went.mMeshes, 2u + sPreparedCells);
            EXPECT_EQ(went.mMaterials, 2u + sPreparedCells);
            EXPECT_EQ(mRing.find(*mContent.mBark), nullptr) << "no model the ring knows of names it";

            fill();
            EXPECT_EQ(placed(), sPlacedCells) << "ground and nothing on it";
            EXPECT_EQ(mRing.getHeldCellCount(), sPreparedCells) << "eleven by eleven cells prepared";
            EXPECT_EQ(mScene.getTables().mMeshes.getLiveCount(), sPreparedCells);
        }

        /// The paging's size rule, per reference: a radius under the threshold at the eye's distance
        /// to the cell is not stood, and the threshold is the eye's and not the chunk's.
        TEST_F(RtxCellRingTest, theSizeRuleIsTheEyesDistanceTimesTheSetting)
        {
            const Placed tree{ .mCell = osg::Vec2i(3, 0),
                .mModel = "tree.nif",
                .mRefNum = ESM::RefNum{ 1, 0 },
                .mPosition = osg::Vec3f(3.5f * sCellSize, 0.5f * sCellSize, 0.0f) };
            const Placed fern{ .mCell = osg::Vec2i(3, 0),
                .mModel = "fern.nif",
                .mRefNum = ESM::RefNum{ 2, 0 },
                .mPosition = osg::Vec3f(3.5f * sCellSize, 0.5f * sCellSize, 0.0f) };
            mStorage.mPlaced = { tree, fern };

            // The eye stands half a cell in, so cell 3 begins two and a half cells away: 20480
            // units, and a hundredth of that is 204.8. The tree's sheet reaches 300 * sqrt(2) and
            // the fern's 20 * sqrt(2).
            mRing.setMinSize(0.01f);
            start();

            EXPECT_EQ(fill().mDistantStatics, 1u) << "the tree clears 204.8 and the fern does not";

            // Nearer, the fern clears too: at a hundredth of 8192 the threshold is 81.92, and the
            // fern's 28.28 still does not — so the threshold is lowered instead.
            mRing.setMinSize(0.001f);
            EXPECT_EQ(walk(mWalked++).mDistantStatics, 2u) << "at 20.48 both clear";
        }

        /// A model the frame lets go of and a delivered cell names again inside the same settled
        /// walk stays lent.
        ///
        /// **The return is stale before it is published.** A settled walk drops the cells that
        /// left the band, and so lets go of their models, before it waits for the thread to read
        /// the cells that entered — and one of those names the same model, read while the frame
        /// still held it. The frame knows the model again from that cell, so the return it wrote
        /// earlier in the walk names a lend the frame holds; published, it had the reader give
        /// the model back under the frame's feet and refill it for the next model.
        TEST_F(RtxCellRingTest, aModelNamedAgainByACellDeliveredInsideTheWaitIsNotGivenBack)
        {
            const Placed near{ .mCell = osg::Vec2i(3, 0),
                .mModel = "tree.nif",
                .mRefNum = ESM::RefNum{ 1, 0 },
                .mPosition = osg::Vec3f(3.5f * sCellSize, 0.5f * sCellSize, 0.0f) };
            const Placed far{ .mCell = osg::Vec2i(9, 0),
                .mModel = "tree.nif",
                .mRefNum = ESM::RefNum{ 2, 0 },
                .mPosition = osg::Vec3f(9.5f * sCellSize, 0.5f * sCellSize, 0.0f) };
            mStorage.mPlaced = { near, far };
            start();

            EXPECT_EQ(fill().mDistantStatics, 1u) << "the near tree, on the one mesh the model has";
            const std::size_t meshes = mScene.getTables().mMeshes.getLiveCount();

            // The eye leaves for a cell from which the near tree's cell is out of the band and the
            // far tree's is in it: the walks that follow let the model go and take it up again.
            around(osg::Vec3f(10.5f * sCellSize, 0.5f * sCellSize, 0.0f), osg::Vec4i(9, -1, 12, 2));
            EXPECT_EQ(fill().mDistantStatics, 0u) << "the far tree stands in the active grid now";
            around(osg::Vec4i(11, -1, 14, 2));
            EXPECT_EQ(walk(mWalked++).mDistantStatics, 1u) << "and outside it, on the model the ring kept";

            // The sweep takes the ground of the band that left — the old band and the new share
            // the eleven cells of one column — and nothing else: the tree's mesh was stamped
            // through, so the scene holds the new band's ground and that one mesh.
            EXPECT_EQ(mExtractor.retire().mMeshes, sPreparedCells - 11u);
            EXPECT_EQ(mScene.getTables().mMeshes.getLiveCount(), meshes) << "one tree mesh, held throughout";

            // Two more walks, so the thread's give-backs of what was returned have run against
            // the reader's own contract — which a model given back while lent breaks loudly.
            walk(mWalked++);
            walk(mWalked++);
            EXPECT_EQ(mScene.getTables().mPlacements.getPlacedCount(), 1u + sPlacedCells);
        }

        /// Unsettled, the ring adopts one cell a frame as the thread delivers them, and a frame
        /// walked twice adopts once.
        TEST_F(RtxCellRingTest, unsettledTheRingAdoptsOneCellAFrame)
        {
            const Placed tree{ .mCell = osg::Vec2i(3, 0),
                .mModel = "tree.nif",
                .mRefNum = ESM::RefNum{ 1, 0 },
                .mPosition = osg::Vec3f(3.5f * sCellSize, 0.5f * sCellSize, 0.0f) };
            mStorage.mPlaced = { tree };
            mRing.setSettled(false);
            start();

            // Eleven by eleven cells to prepare, one adopted a frame: the tree stands somewhere
            // inside the first hundred and twenty-one frames, and never before its cell arrived.
            std::size_t frame = 1;
            std::uint32_t statics = 0;
            for (; frame <= 400 && statics == 0; ++frame)
            {
                statics = walk(frame).mDistantStatics;
                walk(frame);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            EXPECT_EQ(statics, 1u);
            EXPECT_LE(mRing.getHeldCellCount(), frame) << "one cell a frame, and a frame walked twice adopts once";
        }

        /// Settled, a walk adopts the one cell an unsettled walk does and waits for it.
        ///
        /// **No sleep anywhere here, and that is the whole claim.** The test above has to wait on
        /// the wall, because an unsettled walk that finds nothing read adopts nothing. A settled
        /// walk waits for the cell it is about to adopt, so the count after N walks is exactly N.
        ///
        /// **And exactly N and never more**, which is the half that used to be wrong: waiting for
        /// the whole band and adopting all of it put a hundred and twenty-one cells on one frame.
        TEST_F(RtxCellRingTest, settledAWalkWaitsForItsOneCellAndTakesNoMore)
        {
            start();

            for (std::size_t walked = 1; walked <= 20; ++walked)
            {
                walk(mWalked++);
                EXPECT_EQ(mRing.getHeldCellCount(), walked) << "a settled walk adopts one cell and waits for it";
            }

            // And a frame walked twice adopts once, which is the rule both ways.
            const std::size_t held = mRing.getHeldCellCount();
            const std::size_t frame = mWalked++;
            walk(frame);
            walk(frame);
            EXPECT_EQ(mRing.getHeldCellCount(), held + 1);
        }
    }
}
