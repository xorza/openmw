#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Array>
#include <osg/GL>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Image>
#include <osg/Matrix>
#include <osg/MatrixTransform>
#include <osg/Matrixf>
#include <osg/Node>
#include <osg/Quat>
#include <osg/Vec2i>
#include <osg/Vec3f>
#include <osg/Vec4i>
#include <osg/ref_ptr>

#include <components/esm/refid.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm3/loadligh.hpp>
#include <components/esm3/refnum.hpp>
#include <components/misc/constants.hpp>
#include <components/rtx/cellreader.hpp>
#include <components/rtx/cellring.hpp>
#include <components/rtx/cellworld.hpp>
#include <components/rtx/compositequeue.hpp>
#include <components/rtx/extractionstats.hpp>
#include <components/rtx/lightbuilder.hpp>
#include <components/rtx/material.hpp>
#include <components/rtx/mesh.hpp>
#include <components/rtx/prepared.hpp>
#include <components/rtx/refusals.hpp>
#include <components/rtx/result.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/specularlayout.hpp>
#include <components/rtx/textureencoding.hpp>
#include <components/rtx/texturetable.hpp>
#include <components/rtx/texturewrap.hpp>
#include <components/sceneutil/lightcommon.hpp>
#include <components/sceneutil/morphgeometry.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/terrain/objectstorage.hpp>
#include <components/vfs/pathutil.hpp>

#include "support/allocations.hpp"
#include "support/death.hpp"
#include "support/fakeland.hpp"
#include "support/geometry.hpp"
#include "support/graph.hpp"

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

        /// One `LIGH` reference a storage stands, and the record it names. The id is the record's
        /// own, so two lamps with two records are two ids.
        struct Lit
        {
            osg::Vec2i mCell;
            const char* mRecord = nullptr;
            ESM::RefNum mRefNum;
            osg::Vec3f mPosition;
        };

        /// A `LIGH` record reduced the way the engine reduces one: a white lamp of radius 100,
        /// carrying `flags`.
        SceneUtil::LightCommon describeLamp(std::int32_t flags)
        {
            ESM::Light record;
            record.mData.mRadius = 100;
            record.mData.mColor = 0x00FFFFFF;
            record.mData.mFlags = flags;
            return SceneUtil::LightCommon(record);
        }

        /// A storage of a handful of statics and lamps, each in a cell of its own choosing. One
        /// walk of a cell answers both kinds, as the game's does, and a lamp is a reference whose
        /// record `getLight` knows.
        class FewStatics final : public Terrain::ObjectStorage
        {
        public:
            std::vector<Placed> mPlaced;
            std::vector<Lit> mLit;

            /// Whether a walk of a cell throws, which is a reader that fails on its own thread.
            bool mThrows = false;

            /// The records the lamps name: `lit` burns where it stands, `unlit` is off by default,
            /// `flame` flickers and `dark` takes light away.
            std::map<std::string, SceneUtil::LightCommon> mRecords{
                { "lit", describeLamp(0) },
                { "unlit", describeLamp(ESM::Light::OffDefault) },
                { "flame", describeLamp(ESM::Light::Flicker) },
                { "dark", describeLamp(ESM::Light::Negative) },
            };

            std::unique_ptr<Terrain::RefCollector> makeCollector() const override
            {
                return std::make_unique<Terrain::RefCollector>();
            }

            void collect(float, const osg::Vec2i& startCell, ESM::RefId, Terrain::RefKinds kinds,
                Terrain::RefCollector&, std::vector<Terrain::PagedCellRef>& into) const override
            {
                if (mThrows)
                    throw std::runtime_error("a storage that cannot be read");

                into.clear();

                if (Terrain::holds(kinds, Terrain::RefKinds::Paged))
                    for (const Placed& placed : mPlaced)
                        if (placed.mCell == startCell)
                            into.push_back(Terrain::PagedCellRef{
                                .mRefId = ESM::RefId::stringRefId(placed.mModel),
                                .mRefNum = placed.mRefNum,
                                .mPosition = placed.mPosition,
                                .mRotation = placed.mRotation,
                                .mScale = placed.mScale,
                            });

                if (Terrain::holds(kinds, Terrain::RefKinds::Lit))
                    for (const Lit& lamp : mLit)
                        if (lamp.mCell == startCell)
                            into.push_back(Terrain::PagedCellRef{
                                .mRefId = ESM::RefId::stringRefId(lamp.mRecord),
                                .mRefNum = lamp.mRefNum,
                                .mPosition = lamp.mPosition,
                            });
            }

            std::optional<SceneUtil::LightCommon> getLight(const ESM::RefId& id) const override
            {
                const auto found = mRecords.find(id.getRefIdString());
                return found != mRecords.end() ? std::optional(found->second) : std::nullopt;
            }

            /// The record's id doubles as its model here.
            VFS::Path::Normalized getModel(const ESM::RefId& id) const override
            {
                ++mModelsAsked;
                return VFS::Path::Normalized(id.getRefIdString());
            }

            /// How many times `getModel` was asked, which a reader asks once a record.
            mutable int mModelsAsked = 0;
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
                if (path.value() == "meshes/broken.nif")
                {
                    ++mBrokenAsked;
                    return mBroken;
                }
                return nullptr;
            }

            Result<osg::ref_ptr<const osg::Image>, std::string> getImage(const VFS::Path::NormalizedView path) override
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

            /// A template whose triangles name a vertex it does not have, and how often it was
            /// asked for.
            osg::ref_ptr<osg::Group> mBroken = [] {
                osg::ref_ptr<osg::Group> root = new osg::Group;
                root->addChild(makeIndexPastItsVertices());
                return root;
            }();
            std::uint32_t mBrokenAsked = 0;

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

        /// The number of cells some point of which lies nearer than `reach` cells to an eye at the
        /// centre of its own cell — the disc `withinReach` draws. A cell `dx` columns over has its
        /// nearest point `|dx| - 0.5` cells away, and one in the eye's own column has it at nought.
        constexpr std::uint32_t cellsWithin(const float reach)
        {
            const auto nearest
                = [](const int away) { return away == 0 ? 0.0f : static_cast<float>(away < 0 ? -away : away) - 0.5f; };

            std::uint32_t count = 0;
            const int span = static_cast<int>(reach) + 1;
            for (int dx = -span; dx <= span; ++dx)
                for (int dy = -span; dy <= span; ++dy)
                {
                    const float ax = nearest(dx);
                    const float ay = nearest(dy);
                    if (ax * ax + ay * ay < reach * reach)
                        ++count;
                }

            return count;
        }

        /// The placed disc at a reach of four cells, and the prepared disc a cell wider. A square
        /// of nine by nine held twelve more, every one of them behind the air that closes at the
        /// reach.
        constexpr std::uint32_t sPlacedCells = cellsWithin(4.0f);
        constexpr std::uint32_t sPreparedCells = cellsWithin(5.0f);
        static_assert(sPlacedCells == 69 && sPreparedCells == 101);

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

                mRing.setMinSize(0.0f);
                mRing.setSettled(true);

                mAround.mReach = 4.0f * sCellSize;
                mAround.mActiveGrid = osg::Vec4i(-1, -1, 2, 2);
                mAround.mEye = osg::Vec3f(0.5f * sCellSize, 0.5f * sCellSize, 0.0f);
                mAround.mExterior = true;
            }

            /// What `WorldMirror::detach` does, asked of every scenario: the ring told of no
            /// world, its holds given back, the extractor swept — and the scene then empty, or a
            /// row of this world outlived it.
            void TearDown() override
            {
                mRing.follow(WorldAround{});
                mExtractor.detach(mRing);
                EXPECT_TRUE(mScene.isEmpty()) << "a world detached and still standing rows";
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

            /// One frame's walk, which is where the ring adopts, places and stamps. The lists a walk
            /// refills wholesale are emptied first, as a frame empties them.
            ExtractionStats walk(std::size_t frame)
            {
                mScene.clearPlacement();
                mRing.follow(mAround);
                mRing.setFrame(frame);
                const ExtractionStats stats
                    = mExtractor.extractWorld(*mEmpty, osg::Matrixf::identity(), 0, frame, mRing);
                mScene.placements().advance();
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
                total.mLights = last.mLights;
                return total;
            }

            std::uint32_t placed() const { return mScene.placements().getCounts().mPlaced; }

            /// The placement standing the ground of `cell`, which is the one translated to the
            /// cell's centre.
            std::optional<MeshInstance> groundOf(const osg::Vec2i& cell) const
            {
                const osg::Vec3f centre((static_cast<float>(cell.x()) + 0.5f) * sCellSize,
                    (static_cast<float>(cell.y()) + 0.5f) * sCellSize, 0.0f);

                for (const PlacementRow& row : mScene.placements().getRows())
                    if (row.mInstance.isPlaced() && row.mInstance.mTransform.getTrans() == centre)
                        return row.mInstance;

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

            mRing.setSpecularLayout(SpecularLayout::MetalRoughness);
            start();

            const ExtractionStats first = fill();
            EXPECT_EQ(first.mDistantStatics, 3u)
                << "two trees and a fern; the active grid's is the game's and the far one is past the reach";
            EXPECT_EQ(first.mGroundCells, sPlacedCells)
                << "every cell of the reach's ground, the active grid's included";
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
            EXPECT_EQ(mScene.meshes().getMeshPositions(far->mMesh)[0], FakeLand::positionAt(0, 0));
            EXPECT_EQ(mScene.meshes().getMeshPositions(far->mMesh).size(),
                static_cast<std::size_t>(FakeLand::sVerts) * FakeLand::sVerts);
            {
                const Material& material = mScene.materials().getRows()[far->mMaterial];
                EXPECT_EQ(material.mKind, MaterialKind::Terrain);
                EXPECT_EQ(material.mLayers.mCount, 2u);
                EXPECT_TRUE(material.mFlatten);

                // The rock's maps: its normal map in a slot of its own, tiled and read as data, and
                // its `_diffusespec` authored under the layout that reads the alpha as a roughness.
                const std::span<const MaterialLayer> layers = material.mLayers.in(mScene.materials().getLayers());
                EXPECT_EQ(layers[0].mNormal, sNoIndex);
                EXPECT_EQ(layers[0].mFlags, 0u);
                ASSERT_NE(layers[1].mNormal, sNoIndex);
                const TextureRow& normal = mScene.textures().getRows()[layers[1].mNormal];
                EXPECT_EQ(normal.mPath, "textures/rock_nh.dds");
                EXPECT_EQ(normal.mWrap, TextureWrap::Repeat);
                EXPECT_EQ(normal.mEncoding, TextureEncoding::Data);

                // Each slot keeps the image the reader opened on its thread, which is what the
                // upload reads: the frame it lands on opens no file.
                EXPECT_EQ(
                    normal.mImage, mContent.mImages.get(VFS::Path::NormalizedView("textures/rock_nh.dds")).value());
                EXPECT_EQ(mScene.textures().getRows()[layers[1].mDiffuse].mImage,
                    mContent.mImages.get(VFS::Path::NormalizedView("textures/rock_diffusespec.dds")).value());
                EXPECT_EQ(layers[1].mFlags, Shaders::LAYER_AUTHORED | Shaders::LAYER_PARALLAX);
                EXPECT_TRUE(material.mLayersMapped);
            }
            const std::optional<MeshInstance> home = groundOf(osg::Vec2i(0, 0));
            ASSERT_TRUE(home.has_value());
            EXPECT_FALSE(mScene.materials().getRows()[home->mMaterial].mFlatten);
            EXPECT_FALSE(groundOf(osg::Vec2i(5, 0)).has_value()) << "prepared a band out, and not placed";

            // The sheet's origin, lifted five units in the template and then stood as the game
            // stands the reference — which one placement of the three lands on. The cells are
            // walked in their own order, so the tree's is not the first placement.
            const osg::Vec3f expected = osg::Vec3f(0.0f, 0.0f, 5.0f) * gameStands(tree);
            std::size_t standing = 0;
            for (const PlacementRow& row : mScene.placements().getRows())
            {
                const osg::Vec3f stood = osg::Vec3f() * row.mInstance.mTransform;
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
            for (const PlacementRow& row : mScene.placements().getRows())
                if ((osg::Vec3f() * row.mInstance.mTransform - untilted).length() < 0.01f)
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

            // The chunks that asked are flattened as the game's queue flattens them, and the rock
            // reflects, so cell (3, 0) is given a gloss beside its composite.
            CompositeQueue composites;
            while (composites.advance(mScene) > 0)
            {
            }
            EXPECT_NE(mScene.materials().getRows()[far->mMaterial].mDiffuse, sNoIndex);
            EXPECT_NE(mScene.materials().getRows()[far->mMaterial].mSpecular, sNoIndex);

            // The active grid moves over cell (3, 0): its ground shades from its stack from now
            // on, on the same row and with neither of the two images its composite was, the two
            // trees inside the grid are the game's, and the tree at the eye's own cell — outside
            // the grid now — is the ring's.
            around(osg::Vec4i(2, -1, 5, 2));
            walk(mWalked++);
            EXPECT_EQ(placed(), 2u + sPlacedCells) << "the fern and the tree at home stand outside the grid";
            EXPECT_FALSE(mScene.materials().getRows()[far->mMaterial].mFlatten);
            EXPECT_EQ(mScene.materials().getRows()[far->mMaterial].mDiffuse, sNoIndex);
            EXPECT_EQ(mScene.materials().getRows()[far->mMaterial].mSpecular, sNoIndex);
            EXPECT_TRUE(mExtractor.retire().empty());

            // The eye leaves for a cell far away. **The band that left goes on the first walk after
            // the move and the band that arrives comes a cell a walk after it**, so the sweep that
            // follows that one walk is where the meshes nothing stands on go — the two models' and
            // the ground of every cell that left.
            // Under the layout that reads a `_diffusespec`'s alpha as no roughness from here on.
            mRing.setSpecularLayout(SpecularLayout::Ignore);
            around(osg::Vec3f(20.5f * sCellSize, 20.5f * sCellSize, 0.0f), osg::Vec4i(19, 19, 22, 22));
            walk(mWalked++);

            const Retirement went = mExtractor.retire();
            EXPECT_EQ(went.mMeshes, 2u + sPreparedCells);
            EXPECT_EQ(went.mMaterials, 2u + sPreparedCells);

            fill();
            EXPECT_EQ(placed(), sPlacedCells) << "ground and nothing on it";
            EXPECT_EQ(mRing.getHeldCellCount(), sPreparedCells) << "the prepared disc's cells";
            EXPECT_EQ(mScene.meshes().getLiveCount(), sPreparedCells);

            // So the rock there is a diffuse like any, and keeps its normal map.
            const std::optional<MeshInstance> away = groundOf(osg::Vec2i(24, 20));
            ASSERT_TRUE(away.has_value());
            const Material& awayMaterial = mScene.materials().getRows()[away->mMaterial];
            const std::span<const MaterialLayer> awayLayers = awayMaterial.mLayers.in(mScene.materials().getLayers());
            ASSERT_EQ(awayLayers.size(), 2u);
            EXPECT_EQ(awayLayers[1].mFlags, Shaders::LAYER_PARALLAX);
            ASSERT_NE(awayLayers[1].mNormal, sNoIndex);
            EXPECT_EQ(mScene.textures().getRows()[awayLayers[1].mNormal].mPath, "textures/rock_nh.dds");
            EXPECT_TRUE(awayMaterial.mLayersMapped);
        }

        /// The lamps of the cells the game has not loaded stand with their cells: outside the active
        /// grid only, because inside it the game's own graph carries them and a lantern must not be
        /// counted twice; where the record casts at all; and as the light the walk would build from
        /// the graph's own node at the same hour, so a lamp the game loads later is the lamp that
        /// was there. The frame's clock reaches them: a flame at one second is not the flame at
        /// nought.
        ///
        /// **A fake and not a cell, because the shipped content cannot ask about an unlit lamp.**
        /// Every one of the 1559 exterior cells of `Morrowind.esm`, `Tribunal.esm` and
        /// `Bloodmoon.esm` places its lights lit: the off-default flag is an interior's brazier and
        /// a storeroom's torch.
        TEST_F(RtxCellRingTest, lampsStandWithTheirCellsOutsideTheGridAndBurnAtTheFramesHour)
        {
            const osg::Vec3f far(4.5f * sCellSize, 0.5f * sCellSize, 40.0f);
            mStorage.mLit = {
                Lit{ .mCell = osg::Vec2i(4, 0), .mRecord = "flame", .mRefNum = ESM::RefNum{ 7, 0 }, .mPosition = far },
                Lit{ .mCell = osg::Vec2i(3, 0), .mRecord = "unlit", .mRefNum = ESM::RefNum{ 8, 0 } },
                Lit{ .mCell = osg::Vec2i(0, 0), .mRecord = "lit", .mRefNum = ESM::RefNum{ 9, 0 } },
                Lit{ .mCell = osg::Vec2i(7, 0), .mRecord = "lit", .mRefNum = ESM::RefNum{ 10, 0 } },
            };
            mAround.mSimulationTime = 0.0;
            start();

            const ExtractionStats filled = fill();
            ASSERT_EQ(mScene.lights().size(), std::size_t{ 1 })
                << "one lamp is in reach, outside the grid and lit: (4, 0). (0, 0) is the game's, (3, 0) is off "
                   "by default and (7, 0) is out of reach";
            EXPECT_EQ(filled.mLights, 1u) << "and the walk's report counts it";

            const Light stood = mScene.lights().front();
            EXPECT_EQ(stood.mPosition, far);

            // The light the walk builds from the graph's own node, to the bit: one rule, in
            // `makeLight`, phased by the reference number.
            const std::optional<Light> built = makeLight(mStorage.mRecords.at("flame"), far, 0.0, 7).value();
            ASSERT_TRUE(built.has_value());
            EXPECT_EQ(stood.mIntensity, built->mIntensity);
            EXPECT_EQ(stood.mReach, built->mReach);
            EXPECT_EQ(stood.mSourceRadius, built->mSourceRadius);

            // A second later the flame has moved, because the hour reaches it through the walk.
            mAround.mSimulationTime = 1.0;
            mRing.follow(mAround);
            walk(mWalked++);
            ASSERT_EQ(mScene.lights().size(), std::size_t{ 1 });
            EXPECT_NE(mScene.lights().front().mIntensity, stood.mIntensity) << "a flame that stood still";
            EXPECT_EQ(mScene.lights().front().mIntensity,
                makeLight(mStorage.mRecords.at("flame"), far, 1.0, 7).value()->mIntensity);

            // The grid grows over the lamp's cell, and the game's graph is what carries it now.
            around(osg::Vec4i(-1, -1, 5, 2));
            walk(mWalked++);
            EXPECT_TRUE(mScene.lights().empty()) << "a lamp inside the active grid would be counted twice";

            // And back out again, on the frame the grid leaves it.
            around(osg::Vec4i(-1, -1, 2, 2));
            walk(mWalked++);
            EXPECT_EQ(mScene.lights().size(), std::size_t{ 1 });
        }

        /// **What the reader cannot take is refused where its cell is adopted**, on the frame's
        /// thread, which is the one that reports: a model its walk refused, and a lamp that takes
        /// light away. The model is walked once however many references name it, and what else the
        /// cell holds stands. A land texture that does not read is not the reader's to refuse: its
        /// layer stands, and the texture table stands it in and refuses it as it does any texture.
        TEST_F(RtxCellRingTest, whatTheReaderCannotTakeIsRefusedWhereItsCellIsAdopted)
        {
            mContent.mImages.lose("textures/rock_diffusespec.dds");

            const osg::Vec3f inCell(3.5f * sCellSize, 1.5f * sCellSize, 0.0f);
            mStorage.mPlaced = {
                Placed{ .mCell = osg::Vec2i(3, 1),
                    .mModel = "broken.nif",
                    .mRefNum = ESM::RefNum{ 1, 0 },
                    .mPosition = inCell },
                Placed{ .mCell = osg::Vec2i(3, 1),
                    .mModel = "broken.nif",
                    .mRefNum = ESM::RefNum{ 2, 0 },
                    .mPosition = inCell },
                Placed{ .mCell = osg::Vec2i(3, 1),
                    .mModel = "tree.nif",
                    .mRefNum = ESM::RefNum{ 3, 0 },
                    .mPosition = inCell },
            };
            mStorage.mLit = {
                Lit{
                    .mCell = osg::Vec2i(3, 1), .mRecord = "dark", .mRefNum = ESM::RefNum{ 4, 0 }, .mPosition = inCell },
                Lit{ .mCell = osg::Vec2i(3, 1), .mRecord = "lit", .mRefNum = ESM::RefNum{ 5, 0 }, .mPosition = inCell },
            };
            start();

            const ExtractionStats filled = fill();
            EXPECT_EQ(mScene.refusals().count(Refused::Model), 1u);
            EXPECT_EQ(mScene.refusals().count(Refused::Lamp), 1u);
            EXPECT_EQ(mContent.mBrokenAsked, 1u) << "a refused model walked again for its second reference";

            EXPECT_EQ(filled.mDistantStatics, 1u) << "the tree beside the broken models stands";
            ASSERT_EQ(mScene.lights().size(), std::size_t{ 1 }) << "and the lamp beside the dark one burns";
            EXPECT_EQ(mScene.lights().front().mPosition, inCell);

            const Index lost = mScene.textures().findFile(VFS::Path::Normalized("textures/rock_diffusespec.dds"));
            ASSERT_NE(lost, sNoIndex) << "a layer whose image does not read was dropped rather than stood in for";
            EXPECT_EQ(mScene.textures().getRows()[lost].mImage, nullptr)
                << "the slot the upload stands in keeps no image";
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

        /// What the size rule admits is a prefix of the cell's placements, largest first, and a
        /// walk touches only what crossed the prefix's end since the last one. A disabled
        /// reference leaves and returns the moment the script says so, one disabled before its
        /// cell is held arrives that way, and a cleared world forgets them all.
        ///
        /// Five trees of one sheet at scales five to one, so their radii are 424.26 times each —
        /// 2121.3, 1697.1, 1272.8, 848.5 and 424.3. The eye stands half a cell in and cell 3 begins
        /// two and a half cells away, 20480 units, so a setting of `t / 20480` is a threshold of
        /// `t`: 1000 admits three, 600 admits four, 400 all five.
        TEST_F(RtxCellRingTest, theSizeRuleAdmitsAPrefixAndAToggleLandsAtOnce)
        {
            constexpr float sDistance = 2.5f * sCellSize;

            for (std::uint32_t scale = 5; scale >= 1; --scale)
                mStorage.mPlaced.push_back(Placed{ .mCell = osg::Vec2i(3, 0),
                    .mModel = "tree.nif",
                    .mRefNum = ESM::RefNum{ scale, 0 },
                    .mPosition = osg::Vec3f(3.5f * sCellSize, 0.5f * sCellSize, 100.0f * static_cast<float>(scale)),
                    .mScale = static_cast<float>(scale) });
            const Placed elsewhere{ .mCell = osg::Vec2i(3, 1),
                .mModel = "tree.nif",
                .mRefNum = ESM::RefNum{ 6, 0 },
                .mPosition = osg::Vec3f(3.5f * sCellSize, 1.5f * sCellSize, 0.0f),
                .mScale = 5.0f };
            mStorage.mPlaced.push_back(elsewhere);

            // Every placed slot's height, which names the tree standing in it: the template's lift
            // of five is scaled with the reference, so a tree at scale `s` stands at `105 s`.
            const auto standing = [this] {
                std::vector<std::pair<std::size_t, float>> slots;
                const std::span<const PlacementRow> all = mScene.placements().getRows();
                for (std::size_t slot = 0; slot < all.size(); ++slot)
                    if (all[slot].mInstance.isPlaced())
                        slots.emplace_back(slot, all[slot].mInstance.mTransform.getTrans().z());
                return slots;
            };
            const auto heights = [](const std::vector<std::pair<std::size_t, float>>& slots) {
                std::vector<float> lifted;
                for (const auto& [slot, height] : slots)
                    if (height > 50.0f)
                        lifted.push_back(height);
                std::sort(lifted.begin(), lifted.end());
                return lifted;
            };
            const auto trees = [](std::initializer_list<int> scales) {
                std::vector<float> lifted;
                for (const int scale : scales)
                    lifted.push_back(105.0f * static_cast<float>(scale));
                return lifted;
            };
            // Whether every slot of `before` still stands with the same tree in it.
            const auto keeps = [](const std::vector<std::pair<std::size_t, float>>& before,
                                   const std::vector<std::pair<std::size_t, float>>& after) {
                return std::all_of(before.begin(), before.end(),
                    [&](const auto& was) { return std::find(after.begin(), after.end(), was) != after.end(); });
            };

            mRing.setReferenceEnabled(ESM::RefNum{ 6, 0 }, false);
            mRing.setMinSize(1000.0f / sDistance);
            start();

            EXPECT_EQ(fill().mDistantStatics, 3u)
                << "the three largest, and the disabled one in the next cell arrived out";
            const auto three = standing();
            EXPECT_EQ(heights(three), trees({ 3, 4, 5 }));

            mRing.setMinSize(600.0f / sDistance);
            EXPECT_EQ(walk(mWalked++).mDistantStatics, 4u) << "the fourth clears 600";
            const auto four = standing();
            EXPECT_TRUE(keeps(three, four)) << "one add and no drop: the three stand where they stood";
            EXPECT_EQ(heights(four), trees({ 2, 3, 4, 5 }));

            mRing.setMinSize(1000.0f / sDistance);
            EXPECT_EQ(walk(mWalked++).mDistantStatics, 3u);
            EXPECT_TRUE(keeps(three, standing())) << "one drop and no add";

            // A script disables the tree at scale four, inside the prefix: gone before any walk.
            mRing.setReferenceEnabled(ESM::RefNum{ 4, 0 }, false);
            EXPECT_EQ(heights(standing()), trees({ 3, 5 }));
            mRing.setReferenceEnabled(ESM::RefNum{ 4, 0 }, true);
            EXPECT_EQ(heights(standing()), trees({ 3, 4, 5 }));

            // And the tree at scale one, outside it: nothing changes now, and when the threshold
            // falls to admit all five it is the one still kept out.
            mRing.setReferenceEnabled(ESM::RefNum{ 1, 0 }, false);
            EXPECT_EQ(heights(standing()), trees({ 3, 4, 5 }));
            mRing.setMinSize(400.0f / sDistance);
            EXPECT_EQ(walk(mWalked++).mDistantStatics, 4u) << "four of five, the disabled one skipped";
            EXPECT_EQ(heights(standing()), trees({ 2, 3, 4, 5 }));
            mRing.setReferenceEnabled(ESM::RefNum{ 1, 0 }, true);
            EXPECT_EQ(heights(standing()), trees({ 1, 2, 3, 4, 5 })) << "enabled inside the prefix, at once";
            EXPECT_EQ(walk(mWalked++).mDistantStatics, 5u) << "and the walk keeps it";

            // The tree in the next cell was disabled before its cell was held; enabled, it stands.
            mRing.setReferenceEnabled(ESM::RefNum{ 6, 0 }, true);
            EXPECT_EQ(walk(mWalked++).mDistantStatics, 6u);

            // The game moved the tree at scale three, so it is blacklisted: down at once, and a
            // script's word does not raise it — upstream's paging keeps a moved object out of the
            // distance whatever a script says, because where it stands now is the game's.
            mRing.blacklistReference(ESM::RefNum{ 3, 0 });
            EXPECT_EQ(heights(standing()), trees({ 1, 2, 4, 5 }));
            mRing.setReferenceEnabled(ESM::RefNum{ 3, 0 }, true);
            EXPECT_EQ(heights(standing()), trees({ 1, 2, 4, 5 })) << "enabled, and still blacklisted";
            EXPECT_EQ(walk(mWalked++).mDistantStatics, 5u) << "and the walk keeps it down";

            // The world is cleared: what two scripts kept out and what the game blacklisted stand
            // again at once, and the walk keeps it — a list carried into the next game would be
            // the first game's holes in the second one's distance.
            mRing.setReferenceEnabled(ESM::RefNum{ 2, 0 }, false);
            mRing.setReferenceEnabled(ESM::RefNum{ 6, 0 }, false);
            EXPECT_EQ(heights(standing()), trees({ 1, 4, 5 }));
            mRing.forgetReferences();
            EXPECT_EQ(heights(standing()), trees({ 1, 2, 3, 4, 5 }));
            EXPECT_EQ(walk(mWalked++).mDistantStatics, 6u);
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
            const std::size_t meshes = mScene.meshes().getLiveCount();

            // The eye leaves for a cell from which the near tree's cell is out of the band and the
            // far tree's is in it: the walks that follow let the model go and take it up again.
            around(osg::Vec3f(10.5f * sCellSize, 0.5f * sCellSize, 0.0f), osg::Vec4i(9, -1, 12, 2));
            EXPECT_EQ(fill().mDistantStatics, 0u) << "the far tree stands in the active grid now";
            around(osg::Vec4i(11, -1, 14, 2));
            EXPECT_EQ(walk(mWalked++).mDistantStatics, 1u) << "and outside it, on the model the ring kept";

            // The sweep takes the ground of the band that left, and nothing else: the tree's mesh
            // was stamped through, so the scene holds the new band's ground and that one mesh. The
            // old disc and the new share five cells of column 5, which both eyes stand 4.5 cells
            // from: `4.5² + ay² < 5²` holds for a row's `ay` of 0, 0.5 and 1.5, which is `|dy| <=
            // 2`, and fails at 2.5.
            EXPECT_EQ(mExtractor.retire().mMeshes, sPreparedCells - 5u);
            EXPECT_EQ(mScene.meshes().getLiveCount(), meshes) << "one tree mesh, held throughout";

            // Two more walks, so the thread's give-backs of what was returned have run against
            // the reader's own contract — which a model given back while lent breaks loudly.
            walk(mWalked++);
            walk(mWalked++);
            EXPECT_EQ(mScene.placements().getCounts().mPlaced, 1u + sPlacedCells);
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

            // A hundred and one cells to prepare, one adopted a frame: the tree stands somewhere
            // inside the first hundred and one frames, and never before its cell arrived.
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
        /// **And exactly N and never more**: waiting for the whole band and adopting all of it puts a
        /// hundred cells on one frame.
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

        /// A reader that throws is reported to the frame once, and the next world the ring is
        /// pointed at is read by a reader of its own: the failure closed the supply's monitor, and
        /// `follow` opens it again.
        TEST_F(RtxCellRingTest, aWorldFollowedAfterAReaderFailedIsReadAgain)
        {
            mStorage.mThrows = true;
            start();

            // The first walk asks and waits; the reader throws on its thread and closes the
            // monitor, so the wait ends with nothing. A later walk is what takes the failure.
            bool thrown = false;
            for (std::size_t walked = 0; walked < 4 && !thrown; ++walked)
            {
                try
                {
                    walk(mWalked++);
                }
                catch (const std::runtime_error&)
                {
                    thrown = true;
                }
            }
            EXPECT_TRUE(thrown) << "a reader that threw was never reported to the frame";
            EXPECT_EQ(mRing.getHeldCellCount(), 0u);

            // Another worldspace is another reader over the same storages.
            mStorage.mThrows = false;
            mAround.mWorld.mWorldspace = ESM::RefId::stringRefId("elsewhere");
            mRing.follow(mAround);

            fill();
            EXPECT_EQ(mRing.getHeldCellCount(), sPreparedCells)
                << "the supply stayed closed after its reader was replaced";
        }

#ifndef NDEBUG
        /// **A walk that was not told where the world is dies where it happens**, rather than
        /// standing last frame's rings under this frame's eye.
        TEST_F(RtxCellRingTest, aWalkThatWasNotToldWhereTheWorldIsDies)
        {
            start();
            walk(mWalked++);
            expectDies([&] { mExtractor.extractWorld(*mEmpty, osg::Matrixf::identity(), 0, mWalked, mRing); },
                "a call out of its turn");
        }
#endif

        /// Content whose one template is a morph the reader refuses: three vertices over a base
        /// target of two, which `MeshReader::read` throws on.
        class ShortMorph final : public ContentSource
        {
        public:
            osg::ref_ptr<const osg::Node> getTemplate(VFS::Path::NormalizedView) override { return mFace; }
            Result<osg::ref_ptr<const osg::Image>, std::string> getImage(VFS::Path::NormalizedView) override
            {
                return Err{ "no image reads from the file" };
            }

            osg::ref_ptr<osg::Group> mFace = [] {
                osg::ref_ptr<osg::Geometry> source = new osg::Geometry;
                source->setVertexArray(makePositions({ sUnitTriangle[0], sUnitTriangle[1], sUnitTriangle[2] }));
                source->addPrimitiveSet(makeTriangles({ 0, 1, 2 }));

                osg::ref_ptr<SceneUtil::MorphGeometry> morph = new SceneUtil::MorphGeometry;
                morph->setSourceGeometry(source);
                morph->addMorphTarget(new osg::Vec3Array(2), 1.0f);
                morph->addMorphTarget(new osg::Vec3Array(3), 0.0f);

                osg::ref_ptr<osg::Group> root = new osg::Group;
                root->addChild(morph);
                return root;
            }();
        };

        /// A record's model path is built once, however many references name it and however often
        /// the cell is read.
        ///
        /// **Two strings a static reference, on every read of every cell**, is what asking the
        /// storage and correcting the path came to: a town is a few hundred references to a few
        /// dozen models, and a ring reads a band of cells at every crossing. Three references to one
        /// record, read three times: asked once.
        TEST(RtxCellReaderTest, aRecordsModelPathIsBuiltOnceWhateverNamesIt)
        {
            FakeLand land;
            FewStatics storage;
            for (std::uint32_t at = 0; at < 3; ++at)
                storage.mPlaced.push_back(
                    Placed{ .mCell = osg::Vec2i(0, 0), .mModel = "face", .mRefNum = ESM::RefNum{ at, 0 } });
            ShortMorph content;

            CellReader reader(storage, land, content, ESM::Cell::sDefaultWorldspaceId, ~0u);
            for (int pass = 0; pass < 3; ++pass)
                reader.giveBack(reader.read(osg::Vec2i(0, 0), true));

            EXPECT_EQ(storage.mModelsAsked, 1) << "the storage was asked for a record's model more than once";
        }

        /// **A model the walk refuses costs the reader nothing it keeps.** The reference is left
        /// out and named, as `CellReader::read` promises, and the spare the attempt was made in
        /// goes back to the pool: read again, the cell takes no second spare and the template is
        /// held by nothing. Before `Spares::take` took the fill, every read of a cell naming such a
        /// model lost one spare, each holding the template.
        TEST(RtxCellReaderTest, aModelTheWalkRefusesLeavesNoSpareTakenAndNoTemplateHeld)
        {
            FakeLand land;
            FewStatics storage;
            storage.mPlaced.push_back(Placed{ .mCell = osg::Vec2i(0, 0), .mModel = "face" });
            ShortMorph content;

            CellReader reader(storage, land, content, ESM::Cell::sDefaultWorldspaceId, ~0u);

            const int held = content.mFace->referenceCount();
            for (int pass = 0; pass < 3; ++pass)
            {
                PreparedCell& cell = reader.read(osg::Vec2i(0, 0), true);
                EXPECT_TRUE(cell.mModels.empty()) << "a model the walk refused was named";
                EXPECT_TRUE(cell.mRefs.empty());
                reader.giveBack(cell);

                EXPECT_EQ(content.mFace->referenceCount(), held)
                    << "read " << pass + 1 << " kept a hold on the template";
            }
        }
    }
}
