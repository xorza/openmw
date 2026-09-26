#pragma once

#include <atomic>
#include <cstdint>

namespace Crash
{
    /// What the game and its monitor share while both run, read and written through
    /// `std::atomic_ref`: plain words, so both sides map the same bytes.
    struct Heartbeat
    {
        /// Frames the game has drawn. The monitor calls a hang what leaves it unchanged too long.
        alignas(std::atomic_ref<std::uint64_t>::required_alignment) std::uint64_t mFrames;

        /// How long unchanged is a hang, in seconds; nought turns the check off.
        alignas(std::atomic_ref<std::uint32_t>::required_alignment) std::uint32_t mHangSeconds;

        /// Windows only: the function the monitor starts in the game to have it report a hang,
        /// where a POSIX monitor sends a signal instead.
        alignas(std::atomic_ref<std::uint64_t>::required_alignment) std::uint64_t mHangEntry;
    };

    // An atomic that locks takes its lock in the process it runs in, which the other side never
    // sees: across processes only a lock-free one is atomic at all.
    static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free);

    /// The shared page, named after the game's process id, so the monitor finds it from the id
    /// it is started with. Move-only; unmapped as it goes.
    class SharedPage
    {
    public:
        /// The game's side, made before the monitor starts. Null where the system refused.
        static SharedPage create(std::uint64_t process);

        /// The monitor's side of the page `process` made. Null where it is gone or never was.
        static SharedPage open(std::uint64_t process);

        SharedPage() = default;
        SharedPage(SharedPage&& other) noexcept;
        SharedPage& operator=(SharedPage&& other) noexcept;
        SharedPage(const SharedPage&) = delete;
        SharedPage& operator=(const SharedPage&) = delete;
        ~SharedPage();

        Heartbeat* get() const { return mPage; }

    private:
        Heartbeat* mPage = nullptr;
        void* mHandle = nullptr;

        /// The game's id where this side made the page, whose name it gives back if the monitor
        /// never took it: a monitor that did not start leaves it to the game.
        std::uint64_t mMadeFor = 0;
    };
}
