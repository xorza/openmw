#include <chrono>
#include <cstdint>

#include <gtest/gtest.h>

#include <components/rtx/error.hpp>
#include <components/rtx/kernelprogress.hpp>
#include <components/rtx/reconstruction.hpp>
#include <components/rtxvulkan/fogvolume.hpp>
#include <components/rtxvulkan/gbuffer.hpp>
#include <components/rtxvulkan/handles.hpp>
#include <components/rtxvulkan/texture.hpp>
#include <components/rtxvulkan/visibilitypass.hpp>

#include "../support/device/harness.hpp"

namespace Rtx
{
    namespace
    {
        /// The three layouts every kernel of the pass names, which the renderer would own.
        class RtxVisibilityKernelsTest : public Testing::DeviceTest
        {
        protected:
            SetLayout mTextures = TextureArray::describeLayout(getDevice());
            SetLayout mChannels = GBuffer::describeLayout(getDevice());
            SetLayout mVolume = FogVolume::describeLayout(getDevice());
        };

        /// The kernels are counted as they land, the count never goes back, and it reaches the
        /// whole only once the compile is over. The whole is every tuple and the froxels' launch
        /// for each tuple without maps, 16 + 8, or the full tuple and its froxels alone, 1 + 1 —
        /// and beside either the 5 launches no tuple changes.
        ///
        /// **The full tuple alone is watched as it compiles, and the table read off the suite's
        /// renderer**, which made it before any test ran: 36 kernels made again under the layers
        /// were four seconds of this binary for an answer the 7 already give.
        TEST_F(RtxVisibilityKernelsTest, theKernelsAreCountedAsTheyLandAndTheCountEndsWithTheCompile)
        {
            const VisibilityPass pass(
                getDevice(), Testing::getShaderDirectory(), mTextures, mChannels, mVolume, false, false, Reorder::None);
            constexpr std::uint32_t expected = 1 + 1 + 5;

            KernelProgress progress = pass.awaitKernels(std::chrono::milliseconds::zero());
            while (!progress.isDone())
            {
                ASSERT_EQ(progress.mCount, expected);
                ASSERT_LT(progress.mMade, progress.mCount);

                const std::uint32_t before = progress.mMade;
                progress = pass.awaitKernels(std::chrono::milliseconds(1));
                ASSERT_GE(progress.mMade, before);
            }

            EXPECT_EQ(progress.mMade, expected);
            EXPECT_EQ(progress.mCount, expected);
            EXPECT_NO_THROW(pass.awaitKernels());

            ASSERT_TRUE(Testing::getRenderer().getProfile().mSpecializeLaunches);
            const KernelProgress table = Testing::getRenderer().awaitKernels(std::chrono::milliseconds::zero());
            EXPECT_EQ(table.mMade, 16u + 8u + 5u);
            EXPECT_EQ(table.mCount, 16u + 8u + 5u);
        }

        /// A kernel that cannot be made is thrown to every ask, the bounded one included, and not
        /// only to the first: a caller that caught it once must not go on to record from a table
        /// with a hole in it. The constructor returns before any kernel is made, so it is the one
        /// call that does not throw.
        TEST_F(RtxVisibilityKernelsTest, aKernelThatCannotBeMadeIsThrownToEveryAsk)
        {
            const VisibilityPass pass(
                getDevice(), "no-such-directory", mTextures, mChannels, mVolume, false, true, Reorder::None);

            EXPECT_THROW(pass.awaitKernels(), InputError);
            EXPECT_THROW(pass.awaitKernels(), InputError);
            EXPECT_THROW(pass.awaitKernels(std::chrono::milliseconds::zero()), InputError);
        }
    }
}
