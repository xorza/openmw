#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/refusals.hpp>
#include <components/rtx/result.hpp>

#include "support/allocations.hpp"

namespace Rtx
{
    namespace
    {
        /// Each distinct refusal is counted once, by its kind: the same kind, name and reason again
        /// is the same refusal, and a change of any one of the three is another.
        TEST(RtxRefusalsTest, eachDistinctRefusalIsCountedOnceByItsKind)
        {
            Refusals refusals;
            refusals.refuse(Refused::Mesh, "Tri Shape 0", "its triangles name vertex 4 of 4");
            refusals.refuse(Refused::Mesh, "Tri Shape 0", "its triangles name vertex 4 of 4");
            EXPECT_EQ(refusals.count(Refused::Mesh), 1u) << "a repeat is the same refusal";

            refusals.refuse(Refused::Mesh, "Tri Shape 0", "it has 3 normals for 4 vertices");
            refusals.refuse(Refused::Mesh, "Tri Shape 1", "its triangles name vertex 4 of 4");
            EXPECT_EQ(refusals.count(Refused::Mesh), 3u) << "another reason, and another name";

            refusals.refuse(Refused::Model, "Tri Shape 0", "its triangles name vertex 4 of 4");
            EXPECT_EQ(refusals.count(Refused::Model), 1u) << "another kind";
            EXPECT_EQ(refusals.count(Refused::Mesh), 3u);

            refusals.refuse(Refused::Lamp, {}, "a number it is made of is not finite");
            refusals.refuse(Refused::Lamp, {}, "a number it is made of is not finite");
            EXPECT_EQ(refusals.count(Refused::Lamp), 1u) << "an unnamed one repeats as a named one does";

            for (const Refused untouched :
                { Refused::Texture, Refused::SkyLayer, Refused::Moon, Refused::Emitter, Refused::Sprites })
                EXPECT_EQ(refusals.count(untouched), 0u) << static_cast<int>(untouched);

            // What a reader thread held is reported as if it had been reported here.
            const std::vector<Refusal> held{
                Refusal{ .mKind = Refused::Model, .mName = "meshes/x.nif", .mWhy = "its skin names vertex 9 of 8" },
                Refusal{ .mKind = Refused::Model, .mName = "Tri Shape 0", .mWhy = "its triangles name vertex 4 of 4" },
            };
            refusals.refuse(held);
            EXPECT_EQ(refusals.count(Refused::Model), 2u) << "one new, and one already reported";
        }

        /// A refusal met again reaches the heap not at all, which is what lets a value refused on
        /// every frame — a lamp, a particle — be reported where it is met. The first one is spent
        /// and not measured.
        TEST(RtxRefusalsTest, aRefusalMetAgainReachesTheHeapNotAtAll)
        {
            Refusals refusals;
            const std::string name(64, 'x');
            refusals.refuse(Refused::Sprites, name, "a particle's size, colour, place or axis is not a finite number");

            const std::size_t before = Testing::getAllocationCount();
            refusals.refuse(Refused::Sprites, name, "a particle's size, colour, place or axis is not a finite number");
            const std::size_t after = Testing::getAllocationCount();

            EXPECT_EQ(after, before) << after - before << " allocations to meet a refusal again";
            EXPECT_EQ(refusals.count(Refused::Sprites), 1u);
        }

        /// A result holds a value or an error, never both, and says which. The value converts in
        /// as its own type would take it; the error is named where it is made, so a result of two
        /// types that convert into each other is never ambiguous.
        TEST(RtxResultTypeTest, aResultHoldsAValueOrAnErrorAndSaysWhich)
        {
            const Result<std::optional<int>, std::string_view> seven = 7;
            ASSERT_TRUE(seven.isOk());
            EXPECT_EQ(seven.value(), std::optional<int>(7));

            const Result<std::optional<int>, std::string_view> nothing = std::nullopt;
            ASSERT_TRUE(nothing.isOk()) << "nothing is a value, and not an error";
            EXPECT_FALSE(nothing.value().has_value());

            const Result<std::optional<int>, std::string_view> refused = Err{ "no such number" };
            ASSERT_FALSE(refused.isOk());
            EXPECT_EQ(refused.error(), "no such number");

            const Result<std::string_view, std::string_view> same = Err{ std::string_view("an error") };
            EXPECT_FALSE(same.isOk()) << "two types alike, told apart by `Err`";
            const Result<std::string_view, std::string_view> value = std::string_view("a value");
            EXPECT_TRUE(value.isOk());
        }
    }
}
