#include <array>
#include <filesystem>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <apps/rtxtool/perfcontrol.hpp>
#include <components/rtx/error.hpp>

namespace RtxTool
{
    namespace
    {
        /// A fifo with its reading end held open, standing in for the `perf record` that would
        /// normally be on the other side of it.
        class Reader
        {
        public:
            explicit Reader(std::filesystem::path path)
                : mPath(std::move(path))
            {
                std::filesystem::remove(mPath);
                EXPECT_EQ(::mkfifo(mPath.c_str(), 0600), 0);

                // Read-only and non-blocking, which is the one combination that opens a fifo with
                // nobody writing to it yet.
                mHandle = ::open(mPath.c_str(), O_RDONLY | O_NONBLOCK);
                EXPECT_GE(mHandle, 0);
            }

            ~Reader()
            {
                if (mHandle >= 0)
                    ::close(mHandle);
                std::filesystem::remove(mPath);
            }

            const std::filesystem::path& getPath() const { return mPath; }

            /// Everything written so far, which is nothing at all if the writer wrote nothing.
            std::string read() const
            {
                std::array<char, 256> buffer{};
                const ssize_t got = ::read(mHandle, buffer.data(), buffer.size());
                return got > 0 ? std::string(buffer.data(), static_cast<std::size_t>(got)) : std::string();
            }

        private:
            std::filesystem::path mPath;
            int mHandle = -1;
        };

        TEST(RtxPerfControlTest, aBracketedRunSendsPerfTheTwoWordsItListensFor)
        {
            const Reader listening(std::filesystem::temp_directory_path() / "openmw-rtx-perf-control-test");

            {
                PerfControl control(listening.getPath());
                control.enable();
                control.disable();
            }

            EXPECT_EQ(listening.read(), "enable\ndisable\n");
        }

        TEST(RtxPerfControlTest, twoPlacesEachBracketTheirOwnFrames)
        {
            const Reader listening(std::filesystem::temp_directory_path() / "openmw-rtx-perf-control-pair-test");

            PerfControl control(listening.getPath());
            control.enable();
            control.disable();
            control.enable();
            control.disable();

            // The gap between them is a cell being loaded, and perf counts nothing across it.
            EXPECT_EQ(listening.read(), "enable\ndisable\nenable\ndisable\n");
        }

        TEST(RtxPerfControlTest, aRunThatIsNotBeingProfiledSaysNothingAndOpensNothing)
        {
            PerfControl control(std::filesystem::path{});
            control.enable();
            control.disable();

            // The point of the empty path: `bench` holds one of these whether or not perf is there,
            // and the one that is not connected to anything must not go looking for a fifo.
            SUCCEED();
        }

        TEST(RtxPerfControlTest, aStopBeforeTheFirstFrameSaysNothing)
        {
            const Reader listening(std::filesystem::temp_directory_path() / "openmw-rtx-perf-control-stop-test");

            PerfControl control(listening.getPath());
            control.disable();

            EXPECT_EQ(listening.read(), "");
        }

        TEST(RtxPerfControlTest, aFifoWithNobodyReadingItIsNamedRatherThanWaitedOn)
        {
            const std::filesystem::path missing
                = std::filesystem::temp_directory_path() / "openmw-rtx-perf-control-absent";
            std::filesystem::remove(missing);

            PerfControl control(missing);
            EXPECT_THROW(control.enable(), Rtx::Error);
        }
    }
}
