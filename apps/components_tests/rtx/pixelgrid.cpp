#include <array>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/shaders/camera.h>

namespace Rtx
{
    namespace
    {
        using Shaders::shownPixelsFrom;
        using Shaders::tracedPixelUnder;

        /// **Five shown pixels over three traced ones, and two over five.** Worked by hand from the
        /// floors: over three, the centres at 0.3, 0.9, 1.5, 2.1 and 2.7 land on 0, 0, 1, 2 and 2,
        /// so the runs are `[0, 2)`, `[2, 3)` and `[3, 5)`. Over five, the centres at 1.25 and 3.75
        /// land on 1 and 3, so traced pixels 0, 2 and 4 hold no shown pixel and their runs are
        /// empty.
        TEST(RtxPixelGridTest, aTracedPixelsRunIsTheShownPixelsWhoseCentreLandsOnIt)
        {
            EXPECT_EQ((std::vector<std::uint32_t>{ tracedPixelUnder(0, 5, 3), tracedPixelUnder(1, 5, 3),
                          tracedPixelUnder(2, 5, 3), tracedPixelUnder(3, 5, 3), tracedPixelUnder(4, 5, 3) }),
                (std::vector<std::uint32_t>{ 0, 0, 1, 2, 2 }));
            EXPECT_EQ((std::vector<std::uint32_t>{ shownPixelsFrom(0, 5, 3), shownPixelsFrom(1, 5, 3),
                          shownPixelsFrom(2, 5, 3), shownPixelsFrom(3, 5, 3) }),
                (std::vector<std::uint32_t>{ 0, 2, 3, 5 }));

            EXPECT_EQ(tracedPixelUnder(0, 2, 5), 1u);
            EXPECT_EQ(tracedPixelUnder(1, 2, 5), 3u);
            EXPECT_EQ((std::vector<std::uint32_t>{ shownPixelsFrom(0, 2, 5), shownPixelsFrom(1, 2, 5),
                          shownPixelsFrom(2, 2, 5), shownPixelsFrom(3, 2, 5), shownPixelsFrom(4, 2, 5),
                          shownPixelsFrom(5, 2, 5) }),
                (std::vector<std::uint32_t>{ 0, 0, 1, 1, 2, 2 }));
        }

        /// **The runs cover the picture once, at every pair of extents.** The puff composite walks a
        /// traced pixel's run and the display curve asks `tracedPixelUnder` of each shown pixel, and
        /// a pixel the two place differently is a puff the curve reads as absent. Every pair to 128
        /// on each side, and the output extents a player sets against what each upscaler mode
        /// traces for them.
        TEST(RtxPixelGridTest, theRunsCoverEveryShownPixelOnceAtEveryPairOfExtents)
        {
            const auto covers = [](const std::uint32_t shown, const std::uint32_t traced) {
                if (shownPixelsFrom(0, shown, traced) != 0 || shownPixelsFrom(traced, shown, traced) != shown)
                    return false;
                for (std::uint32_t pixel = 0; pixel < shown; ++pixel)
                {
                    const std::uint32_t under = tracedPixelUnder(pixel, shown, traced);
                    if (under >= traced || pixel < shownPixelsFrom(under, shown, traced)
                        || pixel >= shownPixelsFrom(under + 1, shown, traced))
                        return false;
                }
                return true;
            };

            for (std::uint32_t shown = 1; shown <= 128; ++shown)
                for (std::uint32_t traced = 1; traced <= 128; ++traced)
                    ASSERT_TRUE(covers(shown, traced)) << shown << " shown over " << traced << " traced";

            constexpr std::array<std::uint32_t, 7> sides{ 720, 1080, 1440, 1920, 2160, 3840, 7680 };
            constexpr std::array<double, 6> scales{ 1.0, 1.0 / 1.5, 1.0 / 1.724, 0.5, 1.0 / 3.0, 1.5 };
            for (const std::uint32_t shown : sides)
                for (const double scale : scales)
                {
                    const auto traced = static_cast<std::uint32_t>(shown * scale + 0.5);
                    ASSERT_TRUE(covers(shown, traced)) << shown << " shown over " << traced << " traced";
                }
        }
    }
}
