#include "nvml.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>

namespace Rtx
{
    namespace
    {
        /// The driver's own words for how a call went, from `nvml.h`.
        constexpr int sSuccess = 0;
        constexpr int sNotSupported = 3;
        constexpr int sNotFound = 6;
        constexpr int sInsufficientSize = 7;

        /// `nvmlClockType_t` and `nvmlTemperatureSensors_t`.
        constexpr int sGraphicsClock = 0;
        constexpr int sMemoryClock = 2;
        constexpr int sGpuSensor = 0;

        /// The two names the driver ships the one library under.
        constexpr std::array<const char*, 2> sLibraryNames{ "libnvidia-ml.so.1", "nvml.dll" };

        /// Room for the samples of every process a desktop holds on a card, so the driver asks
        /// for more only on a box busier than any this has met.
        constexpr std::size_t sSamplesReserved = 64;

        template <class Function>
        bool load(const Platform::Library::ScopedHandle& library, Function& into, const char* name)
        {
            into = reinterpret_cast<Function>(Platform::Library::find(library.get(), name));
            return into != nullptr;
        }
    }

    Nvml::Nvml()
    {
        static_assert(sizeof(RawSample) == 32, "nvmlProcessUtilizationSample_t is thirty-two bytes");

        for (const char* const name : sLibraryNames)
            if (mLibrary = Platform::Library::ScopedHandle(Platform::Library::open(name)); mLibrary.isOpen())
                break;

        if (!mLibrary.isOpen())
        {
            mAbsence = "the driver's management library did not load";
            return;
        }

        // The versioned names, which are what the header maps the plain ones to, and the event
        // reasons under the name that replaced `ThrottleReasons` or under that one where the
        // driver is older.
        if (!load(mLibrary, mInit, "nvmlInit_v2") || !load(mLibrary, mShutdown, "nvmlShutdown")
            || !load(mLibrary, mHandleByIndex, "nvmlDeviceGetHandleByIndex_v2")
            || !load(mLibrary, mClockInfo, "nvmlDeviceGetClockInfo")
            || !load(mLibrary, mTemperature, "nvmlDeviceGetTemperature")
            || (!load(mLibrary, mEventReasons, "nvmlDeviceGetCurrentClocksEventReasons")
                && !load(mLibrary, mEventReasons, "nvmlDeviceGetCurrentClocksThrottleReasons")))
        {
            mAbsence = "the driver's management library lacks a name this asks for";
            mLibrary = Platform::Library::ScopedHandle();
            return;
        }

        if (const Return began = mInit(); began != sSuccess)
        {
            mAbsence = std::format("the driver's management library would not start, its error {}", began);
            mLibrary = Platform::Library::ScopedHandle();
            return;
        }

        Device device = nullptr;
        if (const Return found = mHandleByIndex(0, &device); found != sSuccess)
        {
            mAbsence = std::format("the driver's management library names no device, its error {}", found);
            mShutdown();
            mLibrary = Platform::Library::ScopedHandle();
            return;
        }
        mDevice = device;

        if (!load(mLibrary, mProcessUtilization, "nvmlDeviceGetProcessUtilization")
            || !load(mLibrary, mProcessName, "nvmlSystemGetProcessName"))
        {
            mUnsampled = "the driver's management library keeps no process samples";
            return;
        }

        // The samples already held are the point to count from: one a process, the latest, at
        // whatever clock it was taken.
        mScratch.resize(sSamplesReserved);
        unsigned count = 0;
        const Return primed = fetch(count);
        if (primed == sNotSupported)
        {
            mUnsampled = "the driver keeps no process samples for this device";
            mProcessUtilization = nullptr;
            return;
        }

        for (std::size_t at = 0; at < count; ++at)
            mCursor = std::max(mCursor, mScratch[at].mStamp);
    }

    Nvml::~Nvml()
    {
        if (isOpen())
            mShutdown();
    }

    GpuClock Nvml::readClock() const
    {
        if (!isOpen())
            return GpuClock{};

        unsigned core = 0;
        unsigned memory = 0;
        unsigned temperature = 0;
        unsigned long long reasons = 0;
        if (mClockInfo(mDevice, sGraphicsClock, &core) != sSuccess
            || mClockInfo(mDevice, sMemoryClock, &memory) != sSuccess
            || mTemperature(mDevice, sGpuSensor, &temperature) != sSuccess
            || mEventReasons(mDevice, &reasons) != sSuccess)
            return GpuClock{};

        return GpuClock::reading(core, memory, temperature, reasons);
    }

    Nvml::Return Nvml::fetch(unsigned& count)
    {
        count = static_cast<unsigned>(mScratch.size());
        Return went = mProcessUtilization(mDevice, mScratch.data(), &count, mCursor);

        // Asked once more with the room it wanted, which is the one growth this ever pays.
        if (went == sInsufficientSize && count > mScratch.size())
        {
            mScratch.resize(count);
            went = mProcessUtilization(mDevice, mScratch.data(), &count, mCursor);
        }

        // Nothing newer than the cursor is what the driver calls not found.
        if (went == sNotFound)
        {
            count = 0;
            return sSuccess;
        }

        if (went != sSuccess)
            count = 0;

        return went;
    }

    void Nvml::readSamples(std::vector<CardSample>& into)
    {
        into.clear();
        if (mProcessUtilization == nullptr)
            return;

        unsigned count = 0;
        if (fetch(count) != sSuccess)
            return;

        std::uint64_t newest = mCursor;
        for (std::size_t at = 0; at < count; ++at)
        {
            const RawSample& raw = mScratch[at];

            // Asked for what is newer than the cursor and told what is; the driver has handed
            // back the sample at the cursor itself, so the read the cursor came from is not
            // counted twice.
            if (raw.mStamp <= mCursor)
                continue;

            into.push_back(
                CardSample{ .mPid = raw.mPid, .mStamp = raw.mStamp, .mHeld = raw.mSm > 0 || raw.mMemory > 0 });
            newest = std::max(newest, raw.mStamp);
        }
        mCursor = newest;
    }

    void Nvml::nameProcess(const std::uint32_t pid, std::string& into) const
    {
        std::array<char, 512> name{};
        if (mProcessName == nullptr || mProcessName(pid, name.data(), static_cast<unsigned>(name.size())) != sSuccess
            || name[0] == '\0')
        {
            into = std::format("pid {}", pid);
            return;
        }

        into = executableOf(name.data());
    }

    std::string_view Nvml::executableOf(const std::string_view called)
    {
        const std::string_view path = called.substr(0, called.find(" -"));
        const std::size_t separator = path.find_last_of("/\\");
        return path.substr(separator == std::string_view::npos ? 0 : separator + 1);
    }
}
