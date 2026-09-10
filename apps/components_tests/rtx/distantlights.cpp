#include <cstdint>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include <osg/NodeVisitor>
#include <osg/Vec2i>
#include <osg/Vec3f>
#include <osg/Vec4i>

#include <components/esm/refid.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm3/loadligh.hpp>
#include <components/misc/constants.hpp>
#include <components/rtx/distantlights.hpp>
#include <components/sceneutil/lightcommon.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/terrain/objectstorage.hpp>
#include <components/vfs/pathutil.hpp>

namespace Rtx
{
    namespace
    {
        /// Where the one lamp stands: past the active grid the eye sits in, and well inside the reach.
        constexpr int sCellX = 4;
        constexpr int sCellY = 0;
        constexpr float sCellSize = static_cast<float>(Constants::CellSizeInUnits);

        /// A storage holding one `LIGH` reference in one cell, and nothing else.
        ///
        /// **A fake and not a cell, because the shipped content cannot ask this question.** Every one of
        /// the 1559 exterior cells of `Morrowind.esm`, `Tribunal.esm` and `Bloodmoon.esm` places its
        /// lights lit: the off-default flag is an interior's brazier and a storeroom's torch. So the one
        /// thing that can put an unlit record in front of the reach is a storage written to do it.
        class OneLamp final : public Terrain::ObjectStorage
        {
        public:
            explicit OneLamp(std::int32_t flags)
            {
                ESM::Light record;
                record.mData.mRadius = 100;
                record.mData.mColor = 0x00FFFFFF;
                record.mData.mFlags = flags;
                mLight.emplace(record);
            }

            void collect(Terrain::RefKind kind, float, const osg::Vec2i& startCell, ESM::RefId,
                std::vector<Terrain::PagedCellRef>& out) const override
            {
                out.clear();
                ++mReadings;

                if (kind != Terrain::RefKind::Lit || startCell != osg::Vec2i(sCellX, sCellY))
                    return;

                out.push_back(Terrain::PagedCellRef{
                    .mRefId = ESM::RefId::stringRefId("lamp"),
                    .mRefNum = ESM::RefNum{ 1, 0 },
                    .mPosition = osg::Vec3f(sCellX * sCellSize, sCellY * sCellSize, 0.0f),
                    .mType = ESM::REC_LIGH,
                });
            }

            std::optional<SceneUtil::LightCommon> getLight(const ESM::RefId&) const override { return mLight; }

            /// How many cells have been read off this, which is what says the memo remembers.
            std::uint32_t getReadings() const { return mReadings; }

            VFS::Path::Normalized getModel(int, const ESM::RefId&) const override { return {}; }

            int getEsmVersion(int) const override { return 0; }

        private:
            std::optional<SceneUtil::LightCommon> mLight;

            /// Mutable because the interface is const and this counts calls rather than answers.
            mutable std::uint32_t mReadings = 0;
        };

        /// Counts the lights a residency hands over.
        ///
        /// **Everything but `take` aborts.** `DistantLights` stands nodes and adopts no row, so a
        /// call to any of the rest would be this residency doing something it has no business doing.
        struct CountLights : osg::NodeVisitor, Collector
        {
            CountLights()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void take(osg::Node& node) override { node.accept(*this); }

            MaterialResolver::Resolved adoptMaterial(const MaterialReading&) override
            {
                ADD_FAILURE() << "the lights adopted a material";
                return MaterialResolver::Resolved{};
            }

            Known& adoptMesh(const osg::Drawable&, const MeshReading&, Index) override
            {
                ADD_FAILURE() << "the lights adopted a mesh";
                return mNothing;
            }

            Known* findMaterial(const osg::StateSet*) override
            {
                ADD_FAILURE() << "the lights looked a material up";
                return nullptr;
            }

            void keepMesh(Known&) override { ADD_FAILURE() << "the lights kept a mesh"; }
            void keepMaterial(Known&) override { ADD_FAILURE() << "the lights kept a material"; }
            void keepOwnedMesh(Index) override { ADD_FAILURE() << "the lights own a mesh"; }
            void keepOwnedMaterial(Index) override { ADD_FAILURE() << "the lights own a material"; }

            Known mNothing;

            void apply(osg::Node& node) override
            {
                if (dynamic_cast<SceneUtil::LightSource*>(&node) != nullptr)
                    ++mFound;

                traverse(node);
            }

            std::uint32_t mFound = 0;
        };

        std::uint32_t stood(std::int32_t flags)
        {
            const OneLamp storage(flags);

            DistantLights lights;

            // The eye at the origin, and the grid the game stands for itself around it — so the lamp
            // four cells out is one the graph route never had a node for.
            lights.follow(WorldAround{ .mStorage = &storage,
                .mWorldspace = ESM::Cell::sDefaultWorldspaceId,
                .mEye = osg::Vec3f(),
                .mReach = sCellSize * 6.0f,
                .mActiveGrid = osg::Vec4i(-1, -1, 2, 2),
                .mOutdoors = true });

            CountLights counted;
            lights.collect(counted);
            return counted.mFound;
        }

        /// A record the content flags off by default stands no light out in the reach either.
        ///
        /// **The rule reaches here through `castsWherePlaced` and not through a second reading of the
        /// flag.** Inside the active grid the game builds no light source for such a record, so the
        /// mirror finds none; out here there is no graph to have skipped it, and a residency that
        /// spelt the rule for itself was a rule that could drift from the one the cell obeys.
        ///
        /// **Both answers on one fixture**, because a count of nought means nothing unless the same
        /// storage with the flag cleared hands one over.
        TEST(RtxDistantLightsTest, aRecordOffByDefaultStandsNoLightInTheReach)
        {
            EXPECT_EQ(stood(0), 1u) << "the reach stood no lamp at all, so this proves nothing";
            EXPECT_EQ(stood(ESM::Light::OffDefault), 0u) << "an unlit lamp was stood out in the reach";
        }

        /// **A cell is read once for the life of the world, and the cells that stood nothing count.**
        /// The reach is thirteen cells across and the active grid takes nine out of the middle, so
        /// one `collect` reads 169 - 9 = 160 cells and the one after it reads none — 159 of which
        /// answered with no light at all, which is the answer the memo has to hold on to. A
        /// structure that remembered only what it found would read those 159 again every frame.
        TEST(RtxDistantLightsTest, aCellIsReadOnceAndTheEmptyOnesAreRememberedToo)
        {
            const OneLamp storage(0);

            DistantLights lights;
            lights.follow(WorldAround{ .mStorage = &storage,
                .mWorldspace = ESM::Cell::sDefaultWorldspaceId,
                .mEye = osg::Vec3f(),
                .mReach = sCellSize * 6.0f,
                .mActiveGrid = osg::Vec4i(-1, -1, 2, 2),
                .mOutdoors = true });

            CountLights first;
            lights.collect(first);
            EXPECT_EQ(storage.getReadings(), 160u) << "thirteen cells square, less the nine the game stands";
            EXPECT_EQ(first.mFound, 1u);

            CountLights again;
            lights.collect(again);
            EXPECT_EQ(storage.getReadings(), 160u) << "a cell was read a second time";
            EXPECT_EQ(again.mFound, 1u) << "what was read once was not handed over twice";

            // What `restart` is for: an id counter that began again leaves every light this holds
            // flickering at a phase from a sequence that has gone.
            lights.restart();

            CountLights afresh;
            lights.collect(afresh);
            EXPECT_EQ(storage.getReadings(), 320u) << "a restart kept what it was told to drop";
            EXPECT_EQ(afresh.mFound, 1u);
        }
    }
}
