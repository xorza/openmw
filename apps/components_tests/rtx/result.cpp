#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/error.hpp>
#include <components/rtxvulkan/result.hpp>

namespace Rtx
{
    namespace
    {
        /// A teardown lets nothing out, whatever it was handed.
        ///
        /// **The one promise the idiom rests on.** Every destructor in this backend does work that
        /// fails when the device is lost, and an exception leaving one is `std::terminate` — which
        /// is how a lost device came to abort the process on top of the fault description it had
        /// just built. A promise with a hole in it would put that back, so this asks about the kind
        /// of throw this tree does not make as well as the kind it does.
        TEST(RtxResultTest, aTearDownLetsNothingOutWhateverItWasHanded)
        {
            EXPECT_NO_THROW(tearDown("a teardown raised", [] { throw Error("the device was lost"); }));
            EXPECT_NO_THROW(tearDown("a teardown raised", [] { throw 7; }));

            bool ran = false;
            EXPECT_NO_THROW(tearDown("a teardown that could not raise", [&] { ran = true; }));
            EXPECT_TRUE(ran) << "the work never ran";
        }

        /// A driver whose list holds `length` and grows by one on each of the first `growths` fills.
        ///
        /// **The race `VK_INCOMPLETE` reports, made to happen.** Vulkan takes the count in one call
        /// and the elements in the next, so a driver is allowed to have more to say by the time the
        /// second arrives — and no real one does it on demand.
        auto aDriverWhoseListGrows(int length, int growths)
        {
            return [length, growths](std::uint32_t* count, int* into) mutable {
                if (into == nullptr)
                {
                    *count = static_cast<std::uint32_t>(length);
                    return VK_SUCCESS;
                }

                const auto offered = static_cast<int>(*count);
                if (growths > 0)
                {
                    --growths;
                    ++length;
                }

                const int written = std::min(offered, length);
                for (int at = 0; at < written; ++at)
                    into[at] = at;
                *count = static_cast<std::uint32_t>(written);

                return written < length ? VK_INCOMPLETE : VK_SUCCESS;
            };
        }

        /// A list that grew between the count and the fill is asked for again, whole.
        ///
        /// **The defect this exists for, and it took a run down.** `vkGetPhysicalDeviceSurfaceFormatsKHR`
        /// offered one more format than it had a moment earlier, `checkVk` called `VK_INCOMPLETE` a
        /// failure, and the process ended during start-up with a swapchain half built.
        TEST(RtxResultTest, anEnumerationAsksAgainWhenTheDriverHadMoreToSay)
        {
            const std::vector<int> once = enumerateVk<int>("a driver that grew once", aDriverWhoseListGrows(3, 1));
            EXPECT_EQ(once, (std::vector<int>{ 0, 1, 2, 3 })) << "the fourth is the one that arrived late";

            // Twice over, because one retry looks exactly like a loop that runs at most once.
            const std::vector<int> twice = enumerateVk<int>("a driver that grew twice", aDriverWhoseListGrows(3, 2));
            EXPECT_EQ(twice, (std::vector<int>{ 0, 1, 2, 3, 4 }));

            // A driver with nothing to add is asked once and answers whole.
            EXPECT_EQ(enumerateVk<int>("a settled driver", aDriverWhoseListGrows(3, 0)), (std::vector<int>{ 0, 1, 2 }));
        }

        /// A list that shrank is trimmed to what the fill actually wrote.
        ///
        /// **The other half of the same race, and the silent one.** A fill that wrote fewer than it
        /// was offered answers `VK_SUCCESS`, so a caller that kept the count it asked with would
        /// hand back elements the driver never touched.
        TEST(RtxResultTest, anEnumerationKeepsOnlyWhatTheFillWrote)
        {
            const auto shrinking = [](std::uint32_t* count, int* into) {
                if (into == nullptr)
                {
                    *count = 4;
                    return VK_SUCCESS;
                }

                into[0] = 11;
                into[1] = 22;
                *count = 2;
                return VK_SUCCESS;
            };

            EXPECT_EQ(enumerateVk<int>("a driver that shrank", shrinking), (std::vector<int>{ 11, 22 }));
        }

        /// An empty list comes back empty, and the fill is never asked for.
        TEST(RtxResultTest, anEnumerationOfNothingAsksForNothing)
        {
            bool filled = false;
            const auto empty = [&](std::uint32_t* count, int* into) {
                filled = filled || into != nullptr;
                *count = 0;
                return VK_SUCCESS;
            };

            EXPECT_TRUE(enumerateVk<int>("a driver with an empty list", empty).empty());
            EXPECT_FALSE(filled) << "a fill into no elements reads as a second count query";
        }

        /// Every element carries the prototype before the driver reads it.
        ///
        /// **What a Vulkan structure needs and a handle does not.** `vkGetPipelineExecutableStatisticsKHR`
        /// reads each element's `sType` on the way in, so a list handed over value-initialised is
        /// one the driver refuses.
        TEST(RtxResultTest, anEnumerationStampsEveryElementBeforeTheDriverReadsIt)
        {
            bool stamped = false;
            const auto reads = [&](std::uint32_t* count, int* into) {
                if (into == nullptr)
                {
                    *count = 2;
                    return VK_SUCCESS;
                }

                stamped = into[0] == 7 && into[1] == 7;
                into[0] = 1;
                into[1] = 2;
                return VK_SUCCESS;
            };

            EXPECT_EQ(enumerateVk<int>("a driver that reads what it is handed", reads, 7), (std::vector<int>{ 1, 2 }));
            EXPECT_TRUE(stamped) << "the driver was handed elements it had not stamped";
        }

        /// A list that never settles ends and names itself, rather than being asked forever.
        ///
        /// **A bound, for the reason `awaitVk` has one.** A driver that lengthens its answer on
        /// every ask cannot be told from one that is working, and a hang says nothing at all.
        TEST(RtxResultTest, anEnumerationThatNeverSettlesEndsAndNamesItself)
        {
            try
            {
                enumerateVk<int>("a driver that never settles", aDriverWhoseListGrows(3, 1000));
                ADD_FAILURE() << "the enumeration returned, so a list that never settles looks like one that did";
            }
            catch (const Error& e)
            {
                EXPECT_NE(std::string(e.what()).find("a driver that never settles"), std::string::npos) << e.what();
                EXPECT_NE(std::string(e.what()).find(std::to_string(sEnumerationTries)), std::string::npos) << e.what();
            }
        }
    }
}
