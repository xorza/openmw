#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <apps/rtxtool/instruments/frametimes.hpp>
#include <components/testing/util.hpp>

// What `PerfControl` sends down a fifo, read off the fifo's other end. POSIX's alone, because a
// fifo is: the tests that need none of one stay in `frametimes.cpp`, and Windows refuses the fifo
// by name in `fifowin32.cpp`.
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
            const Reader listening(TestingOpenMW::outputFilePath("perf-control-test"));

            {
                PerfControl control(listening.getPath());
                control.enable();
                control.disable();
            }

            EXPECT_EQ(listening.read(), "enable\ndisable\n");
        }

        TEST(RtxPerfControlTest, twoPlacesEachBracketTheirOwnFrames)
        {
            const Reader listening(TestingOpenMW::outputFilePath("perf-control-pair-test"));

            PerfControl control(listening.getPath());
            control.enable();
            control.disable();
            control.enable();
            control.disable();

            // The gap between them is a cell being loaded, and perf counts nothing across it.
            EXPECT_EQ(listening.read(), "enable\ndisable\nenable\ndisable\n");
        }

        TEST(RtxPerfControlTest, aStopBeforeTheFirstFrameSaysNothing)
        {
            const Reader listening(TestingOpenMW::outputFilePath("perf-control-stop-test"));

            PerfControl control(listening.getPath());
            control.disable();

            EXPECT_EQ(listening.read(), "");
        }
    }
}
