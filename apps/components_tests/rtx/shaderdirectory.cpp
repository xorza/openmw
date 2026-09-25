#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/shaderdirectory.hpp>
#include <components/testing/util.hpp>

namespace Rtx
{
    namespace
    {
        void writeModule(const std::filesystem::path& file, const std::vector<std::uint8_t>& bytes)
        {
            std::filesystem::create_directories(file.parent_path());
            std::ofstream stream(file, std::ios::binary | std::ios::trunc);
            stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }

        /// Two modules, as a build writes them.
        void writeSet(
            const std::filesystem::path& directory, const std::string& second = "b.spv", const std::uint8_t last = 3)
        {
            writeModule(directory / "a.spv", { 1, 2, last });
            writeModule(directory / second, { 4 });
        }

        TEST(RtxShaderDirectoryTest, theShadersAreReadWithoutTheirSourceUnlessAsked)
        {
            const std::filesystem::path resources("resources");
            EXPECT_EQ(shaderDirectory(resources, false), resources / "rtx" / "shaders");
            EXPECT_EQ(shaderDirectory(resources, true), resources / "rtx" / "shaders-source");
        }

        /// **The digest is the modules and nothing else.** The same modules anywhere are the same
        /// set; one byte, or a module under another name, is another set.
        TEST(RtxShaderDirectoryTest, theDigestIsEveryModulesNameAndBytes)
        {
            const std::filesystem::path root = TestingOpenMW::currentTestDirPath();
            writeSet(root / "here");
            writeSet(root / "elsewhere");
            writeSet(root / "edited", "b.spv", 7);
            writeSet(root / "renamed", "c.spv");

            const std::array<std::uint64_t, 2> here = digestShaders(root / "here");
            EXPECT_EQ(digestShaders(root / "elsewhere"), here);
            EXPECT_NE(digestShaders(root / "edited"), here);
            EXPECT_NE(digestShaders(root / "renamed"), here);
            EXPECT_NE(here, (std::array<std::uint64_t, 2>{ 0, 0 })) << "two modules digested to nothing";

            EXPECT_EQ(digestShaders(root / "missing"), (std::array<std::uint64_t, 2>{ 0, 0 }))
                << "a directory that cannot be read digests to nought";
        }
    }
}
