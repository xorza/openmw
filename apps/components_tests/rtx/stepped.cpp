#include <gtest/gtest.h>

#include <components/rtx/stepped.hpp>

namespace Rtx
{
    namespace
    {
        enum class Turn
        {
            Idle,
            Begun,
            Submitted,
        };

        /// A step is one byte that says where an object stands, moved by `step` and read back by
        /// `get`; `expect` and `step` take any number of steps it may stand at.
        TEST(RtxSteppedTest, aStepMovesForwardFromAnyOfTheStepsNamed)
        {
            Stepped<Turn> turn{ Turn::Idle };
            EXPECT_EQ(turn.get(), Turn::Idle);

            turn.step(Turn::Begun, Turn::Idle);
            EXPECT_EQ(turn.get(), Turn::Begun);

            turn.step(Turn::Submitted, Turn::Idle, Turn::Begun);
            EXPECT_EQ(turn.get(), Turn::Submitted);

            turn.expect(Turn::Submitted);
            turn.expect(Turn::Begun, Turn::Submitted);

            turn.step(Turn::Idle, Turn::Submitted);
            EXPECT_EQ(turn.get(), Turn::Idle);
        }
    }
}
