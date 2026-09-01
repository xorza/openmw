#include <cstdint>
#include <map>
#include <optional>

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

namespace
{
    using namespace Rtx;

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

        void collectReferences(
            float, const osg::Vec2i&, ESM::RefId, std::map<ESM::RefNum, Terrain::PagedCellRef>&) const override
        {
        }

        void collectLights(float, const osg::Vec2i& startCell, ESM::RefId,
            std::map<ESM::RefNum, Terrain::PagedCellRef>& out) const override
        {
            if (startCell != osg::Vec2i(sCellX, sCellY))
                return;

            out.emplace(ESM::RefNum{ 1, 0 },
                Terrain::PagedCellRef{
                    .mRefId = ESM::RefId::stringRefId("lamp"),
                    .mRefNum = ESM::RefNum{ 1, 0 },
                    .mPosition = osg::Vec3f(sCellX * sCellSize, sCellY * sCellSize, 0.0f),
                    .mType = ESM::REC_LIGH,
                });
        }

        std::optional<SceneUtil::LightCommon> getLight(const ESM::RefId&) const override { return mLight; }

        VFS::Path::Normalized getModel(int, const ESM::RefId&) const override { return {}; }

        int getEsmVersion(int) const override { return 0; }

    private:
        std::optional<SceneUtil::LightCommon> mLight;
    };

    /// Counts the lights a residency hands over.
    struct CountLights : osg::NodeVisitor
    {
        CountLights()
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
        }

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
        lights.follow(&storage, ESM::Cell::sDefaultWorldspaceId);
        lights.setReach(sCellSize * 6.0f);
        lights.setOutdoors(true);

        // The eye at the origin, and the grid the game stands for itself around it — so the lamp
        // four cells out is one the graph route never had a node for.
        lights.setViewPoint(osg::Vec3f());
        lights.setActiveGrid(osg::Vec4i(-1, -1, 2, 2));

        CountLights counted;
        lights.collect(counted);
        return counted.mFound;
    }

    /// A record the content flags off by default stands no light out in the reach either.
    ///
    /// **The rule reaches here through `Rtx::castsWherePlaced` and not through a second reading of
    /// the flag.** Inside the active grid the game builds no light source for such a record, so the
    /// mirror finds none; out here there is no graph to have skipped it, and a residency that spelt
    /// the rule for itself was a rule that could drift from the one the cell obeys.
    ///
    /// **Both answers on one fixture**, because a count of nought means nothing unless the same
    /// storage with the flag cleared hands one over.
    TEST(RtxDistantLightsTest, aRecordOffByDefaultStandsNoLightInTheReach)
    {
        EXPECT_EQ(stood(0), 1u) << "the reach stood no lamp at all, so this proves nothing";
        EXPECT_EQ(stood(ESM::Light::OffDefault), 0u) << "an unlit lamp was stood out in the reach";
    }
}
