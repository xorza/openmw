#include <array>
#include <optional>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include <components/rtx/error.hpp>
#include <components/rtx/namedenum.hpp>

namespace Rtx
{
    namespace
    {
        enum class Fruit
        {
            Apple,
            Pear,
            Plum,
        };

        /// Written out of enum order, so an answer that came from the enum rather than the table
        /// would show.
        constexpr NamedEnum sFruitNames{ std::array{
            std::pair{ Fruit::Pear, std::string_view("pear") },
            std::pair{ Fruit::Apple, std::string_view("apple") },
            std::pair{ Fruit::Plum, std::string_view("plum") },
        } };

        /// A table answers a spelling with its value and a value with its spelling, in the order it
        /// was written, and answers a spelling it does not hold with nothing — or, asked to require
        /// one, with every spelling it does hold.
        TEST(RtxNamedEnumTest, aTableAnswersBothWaysAndRefusesWhatItDoesNotName)
        {
            EXPECT_EQ(sFruitNames.named("apple"), Fruit::Apple);
            EXPECT_EQ(sFruitNames.name(Fruit::Plum), "plum");
            EXPECT_EQ(sFruitNames.named("Apple"), std::nullopt) << "a spelling is exact";
            EXPECT_EQ(sFruitNames.named(""), std::nullopt);

            EXPECT_EQ(sFruitNames.values(), (std::array{ Fruit::Pear, Fruit::Apple, Fruit::Plum }));
            EXPECT_EQ(sFruitNames.spellings(), (std::array<std::string_view, 3>{ "pear", "apple", "plum" }));
            EXPECT_EQ(sFruitNames.list(), "pear, apple or plum");

            EXPECT_EQ(sFruitNames.require("pear", "a fruit"), Fruit::Pear);
            try
            {
                sFruitNames.require("fig", "a fruit");
                ADD_FAILURE() << "a fig was taken for a fruit the table names";
            }
            catch (const InputError& error)
            {
                EXPECT_STREQ(error.what(), "\"fig\" is not a fruit: pear, apple or plum");
            }

            // A value the table leaves out has no spelling rather than a wrong one.
            constexpr NamedEnum partial{ std::array{ std::pair{ Fruit::Apple, std::string_view("apple") } } };
            EXPECT_EQ(partial.name(Fruit::Pear), "");
        }

        /// A list covers an enum from nought when it holds each of the first values once, in any
        /// order — not with one of them twice, and not with one missing.
        TEST(RtxNamedEnumTest, aListCoversFromNoughtWithEachFirstValueOnceInAnyOrder)
        {
            EXPECT_TRUE(coversFromNought(std::array{ Fruit::Plum, Fruit::Apple, Fruit::Pear }));
            EXPECT_TRUE(coversFromNought(std::array{ Fruit::Pear, Fruit::Apple }));
            EXPECT_FALSE(coversFromNought(std::array{ Fruit::Apple, Fruit::Apple, Fruit::Pear }));
            EXPECT_FALSE(coversFromNought(std::array{ Fruit::Pear, Fruit::Plum }));
        }
    }
}
