#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace Rtx
{
    /// Runs `body(index)` for every index below `count`, over as many threads as there is work for,
    /// and comes back when the last of them is done. One shot, unlike `Rtx::Worker`. A hand takes
    /// the next index whenever it is free, so a turn that costs ten times its neighbour delays
    /// nothing but itself. The first exception is rethrown and every index is still attempted,
    /// because a caller that asked for a batch wants to know about the batch. Each hand gets its
    /// own copy of both callables, as the standard's parallel algorithms do, so a body with state
    /// of its own is not a data race.
    ///
    /// @param equip what each hand holds for as long as it runs, built on that hand's own thread
    ///        and destroyed there — how the Vulkan backend files a validation message under the
    ///        thread that asked for the work.
    template <class Equip, class Body>
    void runInParallel(const std::size_t count, Equip equip, Body body)
    {
        if (count == 0)
            return;

        std::atomic<std::size_t> next{ 0 };
        std::mutex kept;
        std::exception_ptr failed;

        {
            const auto hands = std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, count);

            std::vector<std::jthread> running;
            running.reserve(hands);
            for (std::size_t at = 0; at < hands; ++at)
                running.emplace_back([&next, &kept, &failed, count, equip, body]() mutable {
                    // Held for the hand's whole run and read by nothing: what it is for is its life.
                    [[maybe_unused]] const auto held = equip();

                    for (std::size_t index = next++; index < count; index = next++)
                    {
                        try
                        {
                            body(index);
                        }
                        catch (...)
                        {
                            const std::lock_guard<std::mutex> hold(kept);
                            if (failed == nullptr)
                                failed = std::current_exception();
                        }
                    }
                });
        }

        if (failed != nullptr)
            std::rethrow_exception(failed);
    }
}
