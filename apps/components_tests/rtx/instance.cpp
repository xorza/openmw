#include <span>
#include <string>

#include <gtest/gtest.h>

#include <components/rtxvulkan/instance.hpp>
#include <components/rtxvulkan/validation.hpp>

#include "support/instanceobstacle.hpp"

namespace Rtx
{
    namespace
    {
        /// Object names are what make a capture readable, and a capture is most wanted on a run that
        /// is not carrying the layers — so the two are enabled independently. Needs its own instance:
        /// the shared harness always asks for validation.
        TEST(RtxInstanceTest, objectNamesDoNotNeedTheValidationLayers)
        {
            if (const std::string obstacle = Testing::findInstanceObstacle(); !obstacle.empty())
                GTEST_SKIP() << obstacle;

            // Its own instance rather than the harness's, because what is being asserted is what an
            // unvalidated one carries — and the harness's comes with a device this does not need.
            const Instance instance{ ValidationOptions{}, std::span<const char* const>{} };

            EXPECT_EQ(instance.getValidationLog(), nullptr);
#ifdef OPENMW_RTX_DEBUG_NAMES
            EXPECT_TRUE(instance.hasDebugUtils());
#else
            EXPECT_FALSE(instance.hasDebugUtils());
#endif
        }
    }
}
