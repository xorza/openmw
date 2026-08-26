#include "harness.hpp"

#include <gtest/gtest.h>

namespace
{
    /// Closes every device the binary cached, after the last test and before `main` returns.
    ///
    /// **Two Vulkan devices destroyed after `main` has returned abort inside the validation layer**,
    /// with no message and no stack of ours on it. One pair survives static destruction and a second
    /// does not — reproduced with nothing in the process but two instances left to exit — and this
    /// binary keeps up to four: a raw device for the tests that drive Vulkan directly and a
    /// `Renderer` for the pixel suite, each in a validated and an unvalidated flavour. Closing them
    /// here is both the fix and where they belonged: a cache that lives for the run should end with
    /// the run, not with the process.
    class DeviceTeardown : public ::testing::Environment
    {
        void TearDown() override
        {
            // Renderers before raw devices, which is the order they were built in; neither depends
            // on the other.
            for (const bool validation : { true, false })
            {
                Rtx::Testing::Details::rendererCache(validation).release();
                Rtx::Testing::Details::harnessCache(validation).release();
            }
        }
    };

    // Before `main`, because gtest only tears down environments registered before the run starts.
    [[maybe_unused]] const bool sRegistered = [] {
        ::testing::AddGlobalTestEnvironment(new DeviceTeardown);
        return true;
    }();
}
