#pragma once

#include <filesystem>

namespace Rtx
{
    /// Where the driver keeps its compiled code of one set of shaders: a directory of its own beside
    /// them, and only the current one.
    ///
    /// **Of its own**, because the driver otherwise shares one cache with every program on the
    /// machine and prunes it at a size limit, so another program's shaders evict these. **Beside the
    /// shaders**, so it goes with the build that made them. **One per set of shaders**, named by
    /// `digestShaders` of the modules the renderer reads, because the driver keys its entries on the
    /// bytes it is handed and never drops one: a directory each build kept across its shader edits
    /// grew past a gigabyte. A changed set is a new directory, and `sweep` removes the old one.
    ///
    /// **Nothing here settles which code the driver runs, and nothing has to.** The driver builds a
    /// launch's code again from its own profile and swaps it in at a frame of some processes, and
    /// the build pins every shader's float arithmetic (`Rtx::pinFloatArithmetic`) so that both codes
    /// trace the same frame.
    class DriverCache
    {
    public:
        /// The cache of the modules in `shaders`, its directory made where there is none.
        explicit DriverCache(const std::filesystem::path& shaders);

        const std::filesystem::path& getDirectory() const { return mDirectory; }

        /// Points the driver at the directory, through the variables the NVIDIA driver reads, with a
        /// size far past what one set of these shaders takes, so the driver prunes nothing. Before
        /// anything makes a device, because the driver reads them once. A driver that reads none of
        /// them keeps its own cache.
        void applyToDriver() const;

        /// Removes every other cache beside the same shaders: modules that are no longer there.
        /// Where one will not go — another process still reads it — it stays for the next.
        void sweep() const;

    private:
        std::filesystem::path mRoot;
        std::filesystem::path mDirectory;
    };
}
