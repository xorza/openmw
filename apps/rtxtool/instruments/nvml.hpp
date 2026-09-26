#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <components/platform/library.hpp>

#include "gpuclock.hpp"

namespace RtxTool
{
    /// One of the driver's own samples of a process on the card: which process, the driver's
    /// clock when it was taken, and whether the process had the SMs or the memory busy in it.
    struct CardSample
    {
        std::uint32_t mPid = 0;
        std::uint64_t mStamp = 0;
        bool mHeld = false;
    };

    /// NVIDIA's management library, which is what `nvidia-smi` reads, opened by name at run time
    /// and asked directly. **Opened and never linked**, because only one vendor's driver ships it
    /// and a build must start on any card; a box without it answers no clock and no card line
    /// rather than failing. **Asked directly and never through the tool**, because a reading
    /// through `nvidia-smi` forks a process with a world loaded, tens of milliseconds a time, and
    /// four a second was as often as that could be afforded — the driver's process samples come
    /// five times a second and are lost between polls slower than that.
    ///
    /// Device nought, which is the line `nvidia-smi` prints first and the card this fork has run
    /// on; a box with two cards is one this has not met.
    class Nvml
    {
    public:
        Nvml();
        ~Nvml();
        Nvml(const Nvml&) = delete;
        Nvml& operator=(const Nvml&) = delete;

        bool isOpen() const { return mDevice != nullptr; }

        /// Why the card cannot be asked, or empty where it can.
        std::string_view describeAbsence() const { return mAbsence; }

        /// Whether the driver's process samples can be read, and if not why: the library's own
        /// absence where it is not open, the device's want of samples where it is.
        bool hasSamples() const { return mProcessUtilization != nullptr; }
        std::string_view describeUnsampled() const { return isOpen() ? mUnsampled : mAbsence; }

        /// One reading of the card's clocks, its temperature and what holds its clock back, or
        /// one that answered nothing where the library is not open or the driver refused.
        GpuClock readClock() const;

        /// Every process sample the driver took since the last call, into `into`, which is cleared
        /// first. **The samples the driver held when this was opened are the point counting starts
        /// from**, so a process that last drew a minute before the run is not read as drawing in
        /// it. The driver hands back one sample a process, the latest, so a caller polling slower
        /// than the driver samples loses the ones between.
        void readSamples(std::vector<CardSample>& into);

        /// What the process the driver numbers `pid` is called — its executable's own name without
        /// the directory — into `into`; `pid <n>` where the process is gone.
        void nameProcess(std::uint32_t pid, std::string& into) const;

        /// The executable's own name in what the driver calls a process: what stands before the
        /// first option, without its directory. **Before the first option and not the whole**,
        /// because what the driver hands back is `argv[0]`, and a browser writes its whole command
        /// line into that to title its processes — `nvidia-smi pmon` prints the tail of a path
        /// inside an argument for one. **Not the first word either**, because a directory may
        /// hold a space and an option begins with a dash.
        static std::string_view executableOf(std::string_view called);

    private:
        /// `nvmlProcessUtilizationSample_t`, laid out as the driver writes it.
        struct RawSample
        {
            std::uint32_t mPid;
            std::uint64_t mStamp;
            std::uint32_t mSm;
            std::uint32_t mMemory;
            std::uint32_t mEncoder;
            std::uint32_t mDecoder;
        };

        using Device = void*;
        using Return = int;

        /// Fills `mScratch` with the samples newer than `mCursor`, answering the driver's own
        /// word for how it went.
        Return fetch(unsigned& count);

        Platform::Library::ScopedHandle mLibrary;
        Device mDevice = nullptr;
        std::string mAbsence;
        std::string_view mUnsampled;

        Return (*mInit)() = nullptr;
        Return (*mShutdown)() = nullptr;
        Return (*mHandleByIndex)(unsigned, Device*) = nullptr;
        Return (*mClockInfo)(Device, int, unsigned*) = nullptr;
        Return (*mTemperature)(Device, int, unsigned*) = nullptr;
        Return (*mEventReasons)(Device, unsigned long long*) = nullptr;
        Return (*mProcessUtilization)(Device, RawSample*, unsigned*, unsigned long long) = nullptr;
        Return (*mProcessName)(unsigned, char*, unsigned) = nullptr;

        /// The driver's clock at the newest sample read, which the next read counts from.
        std::uint64_t mCursor = 0;

        /// Room for the driver's answer, grown to what it asked for the once it asked.
        std::vector<RawSample> mScratch;
    };
}
