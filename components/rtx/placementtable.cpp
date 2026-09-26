#include "placementtable.hpp"

#include <cassert>
#include <cstddef>
#include <span>
#include <vector>

#include <osg/BoundingBox>

namespace Rtx
{
    InstanceCounts PlacementTable::shareOf(const PlacementRow& row)
    {
        const MeshInstance& placed = row.mInstance;
        const Material::Traversed& worn = row.mWorn;
        const PlacedTraversal traversed = worn.placedAt(placed.mOpacity);

        return InstanceCounts{
            .mPlaced = 1,
            .mCutout = traversed.mCutout ? 1u : 0u,
            .mWater = worn.mKind == MaterialKind::Water ? 1u : 0u,
            .mMedium = traversed.mMedium ? 1u : 0u,
            .mAdditive = traversed.mAdditive ? 1u : 0u,
            .mFirstPerson = placed.mClass == InstanceClass::FirstPerson ? 1u : 0u,
            .mMapped = worn.mMapped ? 1u : 0u,
        };
    }

    void PlacementTable::count(const Index slot)
    {
        const InstanceCounts share = shareOf(mRows.at(slot));
        mCounts.mPlaced += share.mPlaced;
        mCounts.mCutout += share.mCutout;
        mCounts.mWater += share.mWater;
        mCounts.mMedium += share.mMedium;
        mCounts.mAdditive += share.mAdditive;
        mCounts.mFirstPerson += share.mFirstPerson;
        mCounts.mMapped += share.mMapped;

        if (share.mMedium + share.mAdditive > 0)
            mPresent.addMakingRoom(slot);
    }

    void PlacementTable::discount(const Index slot)
    {
        const InstanceCounts share = shareOf(mRows.at(slot));
        mCounts.mPlaced -= share.mPlaced;
        mCounts.mCutout -= share.mCutout;
        mCounts.mWater -= share.mWater;
        mCounts.mMedium -= share.mMedium;
        mCounts.mAdditive -= share.mAdditive;
        mCounts.mFirstPerson -= share.mFirstPerson;
        mCounts.mMapped -= share.mMapped;

        if (share.mMedium + share.mAdditive > 0)
        {
            mPresent.remove(slot);
            mPresent.compact();
        }
    }

    Index PlacementTable::add(const MeshInstance& instance, const Material::Traversed& worn)
    {
        // Standing where it is, not arriving from wherever the last tenant left. A reused slot
        // would otherwise inherit a previous transform from something else entirely, and a motion
        // vector built from that points across the frame.
        const Index slot = mRows.take(PlacementRow{
            .mInstance = instance,
            .mPrevious = instance.mTransform,
            .mWorn = worn,
        });

        link(slot, instance.mMaterial);
        count(slot);

        mMoved.push_back(slot);
        return slot;
    }

    void PlacementTable::link(const Index slot, const Index material)
    {
        if (material == sNoIndex)
            return;

        // Grown with the materials rather than with the slots, on the arrival that first names a
        // material this many: the table of materials grows on the same frame.
        if (material >= mFirstWearing.size())
            mFirstWearing.resize(std::size_t{ material } + 1, sNoIndex);

        PlacementRow& row = mRows.at(slot);
        const Index first = mFirstWearing[material];
        row.mNextWearing = first;
        row.mPrevWearing = sNoIndex;
        if (first != sNoIndex)
            mRows.at(first).mPrevWearing = slot;
        mFirstWearing[material] = slot;
    }

    void PlacementTable::unlink(const Index slot, const Index material)
    {
        if (material == sNoIndex)
            return;

        PlacementRow& row = mRows.at(slot);
        const Index next = row.mNextWearing;
        const Index previous = row.mPrevWearing;
        if (previous != sNoIndex)
            mRows.at(previous).mNextWearing = next;
        else
            mFirstWearing[material] = next;
        if (next != sNoIndex)
            mRows.at(next).mPrevWearing = previous;

        row.mNextWearing = sNoIndex;
        row.mPrevWearing = sNoIndex;
    }

    void PlacementTable::rewriteWearing(const Index material, const Material::Traversed& worn)
    {
        if (material >= mFirstWearing.size())
            return;

        for (Index slot = mFirstWearing[material]; slot != sNoIndex; slot = mRows.at(slot).mNextWearing)
        {
            discount(slot);
            mRows.at(slot).mWorn = worn;
            count(slot);
            mMoved.push_back(slot);
        }
    }

    void PlacementTable::fade(const Index slot, const float opacity)
    {
        PlacementRow& row = mRows.at(slot);
        assert(row.mInstance.isPlaced() && "a slot nothing stands in");
        assert(row.mInstance.mStander == Stander::Walk && "a fade of a placement the ring stood");

        if (row.mInstance.mOpacity == opacity)
            return;

        // Counted again, because whether a cutout is asked its question turns on the opacity.
        discount(slot);
        row.mInstance.mOpacity = opacity;
        count(slot);

        mMoved.push_back(slot);
    }

    bool PlacementTable::move(const Index slot, const osg::Matrixf& transform)
    {
        PlacementRow& row = mRows.at(slot);
        assert(row.mInstance.isPlaced() && "a slot nothing stands in");
        assert(row.mInstance.mStander == Stander::Walk && "a move of a placement the ring stood");

        if (row.mInstance.mTransform == transform)
            return false;

        row.mInstance.mTransform = transform;
        mMoved.push_back(slot);
        return true;
    }

    void PlacementTable::drop(const Index slot, const Stander by)
    {
        PlacementRow& row = mRows.at(slot);
        assert(row.mInstance.isPlaced() && "a slot dropped twice, or one nothing stood in");
        assert(row.mInstance.mStander == by && "a slot dropped by a stander that did not stand it");

        unlink(slot, row.mInstance.mMaterial);
        discount(slot);

        // Emptied, so a slot keeps no link: what the row holds after this is what `isPlaced`
        // says, and nothing else reads it until `add` writes it whole.
        row = PlacementRow{};

        mRows.free(slot);
        mMoved.push_back(slot);
    }

    void PlacementTable::describePresences(
        std::span<const MeshRange> meshes, std::vector<Shaders::GpuPresence>& into) const
    {
        into.clear();
        into.reserve(mPresent.getSlots().size());

        for (const Index slot : mPresent.getSlots())
        {
            const PlacementRow& row = mRows.at(slot);
            const MeshInstance& placed = row.mInstance;
            const InstanceCounts share = shareOf(row);

            const osg::BoundingBoxf& box = meshes[placed.mMesh].mBounds;
            if (!box.valid())
                continue;

            osg::BoundingBoxf world;
            for (unsigned int corner = 0; corner < 8; ++corner)
                world.expandBy(box.corner(corner) * placed.mTransform);

            into.push_back(Shaders::GpuPresence{
                .mCentre = world.center(),
                .mRadius = world.radius(),
                .mKinds = (share.mAdditive > 0 ? Shaders::PRESENCE_ADDITIVE : 0u)
                    | (share.mMedium > 0 ? Shaders::PRESENCE_MEDIUM : 0u)
                    | (share.mFirstPerson > 0 ? Shaders::PRESENCE_EVERYWHERE : 0u),
            });
        }
    }

    void PlacementTable::advance()
    {
        for (const Index slot : mMoved)
        {
            PlacementRow& row = mRows.at(slot);
            row.mPrevious = row.mInstance.mTransform;
        }

        // Swapped and not copied: the two lists trade buffers, and neither allocates on the frame.
        mSettled.swap(mMoved);
        mMoved.clear();
    }
}
