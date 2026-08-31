#pragma once

#include <memory>

#include <gtest/gtest.h>

namespace RtxTool
{
    class Content;
    class World;

    /// The base of every test that needs Morrowind installed.
    ///
    /// **The content files are read once for the binary, and every test stands its own world on
    /// them.** That is what the split between `Content` and `World` buys: reading and merging the
    /// content costs some eighty milliseconds and answers the same thing every time, while standing
    /// a world on the result costs a tenth of one. Twenty-seven tests reading the installation each
    /// came to two and a half seconds of a twenty-four second run.
    ///
    /// **A shared world would be worse than slow.** `pageTerrain` and `pageStatics` are read when
    /// the terrain is built and ignored by a world that has already built it, so a second test
    /// asking for a paged world would silently get the first test's gridded one. A world per test
    /// makes that unrepresentable rather than remembered.
    ///
    /// A machine without the game skips, with the reason the installation could not be opened.
    class InstallationTest : public ::testing::Test
    {
    protected:
        /// Opens the installation, or skips the test where there is none.
        void SetUp() override;

        /// What the content files say, read once for the binary.
        const Content& getContent() const { return *mContent; }

        /// This test's world, stood up on the first ask.
        World& getWorld();

        /// Another world over the same content, for a test that compares two of them.
        std::unique_ptr<World> openWorld() const;

    private:
        const Content* mContent = nullptr;
        std::unique_ptr<World> mWorld;
    };
}
