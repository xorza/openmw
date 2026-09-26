#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Vec3f>

#include <components/esm/refid.hpp>
#include <components/esm3/refnum.hpp>
#include <components/terrain/objectstorage.hpp>

#include "../rtx/allocations.hpp"

namespace Terrain
{
    namespace
    {
        PagedCellRef at(std::uint32_t index, float x)
        {
            return PagedCellRef{ .mRefId = ESM::RefId::stringRefId("tree"),
                .mRefNum = ESM::RefNum{ index, 0 },
                .mPosition = osg::Vec3f(x, 0.0f, 0.0f) };
        }

        /// What the stack makes of what a walk said is what a map made of the same words: each
        /// reference's last, where it was not a deletion, in reference order.
        ///
        /// **The words in the order the content files stack.** Reference 3 is placed at 1 and moved
        /// to 2 by a later file; reference 1 is placed and deleted; reference 2 is placed; reference
        /// 5 is deleted and placed again at 7 by a later file still. What stands is 2 at 0, 3 at 2
        /// and 5 at 7, sorted by reference number and not by the order they were said in.
        TEST(TerrainRefStackTest, aReferencesLastWordStandsUnlessItDeletes)
        {
            RefStack stack;
            stack.assign(ESM::RefNum{ 3, 0 }, at(3, 1.0f));
            stack.assign(ESM::RefNum{ 1, 0 }, at(1, 0.0f));
            stack.assign(ESM::RefNum{ 2, 0 }, at(2, 0.0f));
            stack.erase(ESM::RefNum{ 1, 0 });
            stack.erase(ESM::RefNum{ 5, 0 });
            stack.assign(ESM::RefNum{ 3, 0 }, at(3, 2.0f));
            stack.assign(ESM::RefNum{ 5, 0 }, at(5, 7.0f));

            std::vector<PagedCellRef> into;
            stack.reduceInto(into);

            ASSERT_EQ(into.size(), 3u);
            EXPECT_EQ(into[0].mRefNum, (ESM::RefNum{ 2, 0 }));
            EXPECT_EQ(into[1].mRefNum, (ESM::RefNum{ 3, 0 }));
            EXPECT_EQ(into[1].mPosition.x(), 2.0f) << "the later file's move";
            EXPECT_EQ(into[2].mRefNum, (ESM::RefNum{ 5, 0 }));
            EXPECT_EQ(into[2].mPosition.x(), 7.0f) << "placed again after it was deleted";

            // **And the next cell costs nothing it has already held.** Emptied and said to again,
            // no more than before, into a result that kept its room.
            stack.clear();
            into.clear();
            const std::size_t before = Rtx::Testing::getAllocationCount();
            stack.assign(ESM::RefNum{ 9, 0 }, at(9, 0.0f));
            stack.erase(ESM::RefNum{ 9, 0 });
            stack.assign(ESM::RefNum{ 8, 0 }, at(8, 0.0f));
            stack.reduceInto(into);
            EXPECT_EQ(Rtx::Testing::getAllocationCount() - before, 0u);
            ASSERT_EQ(into.size(), 1u);
            EXPECT_EQ(into[0].mRefNum, (ESM::RefNum{ 8, 0 }));
        }
    }
}
