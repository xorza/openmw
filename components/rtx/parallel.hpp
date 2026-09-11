#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace Rtx
{
    /// Runs `body(index)` for every index below `count`, over as many threads as there is work for,
    /// and comes back when the last of them is done.
    ///
    /// **One shot, and nothing like `Rtx::Worker`.** A worker is a thread that outlives the call
    /// that started it and waits on a channel. This is a loop whose turns happen to be able to run
    /// beside each other, so there is no stop token, no queue and nothing to join later.
    ///
    /// **An index and not a share each.** A hand takes the next index whenever it is free, so a
    /// turn that costs ten times its neighbour delays nothing but itself. Every hand runs the same
    /// loop and the atomic is the whole of the sharing.
    ///
    /// **The first exception and not the last**, so what is rethrown names what actually went wrong
    /// rather than whichever hand finished after it. Every index is still attempted: a turn that
    /// threw stops that turn and not the run, because a caller that asked for a batch wants to know
    /// about the batch.
    ///
    /// @param equip what each hand holds for as long as it runs, built on that hand's own thread
    ///        and destroyed there. The Vulkan backend files a validation message under the thread
    ///        that asked for the work this way — the layers report on the calling thread, and a
    ///        message left filed under a hand is one nobody collects.
    template <class Equip, class Body>
    void runInParallel(const std::size_t count, Equip equip, Body body)
    {
        if (count == 0)
            return;

        std::atomic<std::size_t> next{ 0 };
        std::mutex kept;
        std::exception_ptr failed;

        const auto hand = [&] {
            // Held for the hand's whole run and read by nothing: what it is for is its life.
            [[maybe_unused]] const auto held = equip();

            for (std::size_t at = next++; at < count; at = next++)
            {
                try
                {
                    body(at);
                }
                catch (...)
                {
                    const std::lock_guard<std::mutex> hold(kept);
                    if (failed == nullptr)
                        failed = std::current_exception();
                }
            }
        };

        {
            const auto hands = std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, count);

            std::vector<std::jthread> running;
            running.reserve(hands);
            for (std::size_t at = 0; at < hands; ++at)
                running.emplace_back(hand);
        }

        if (failed != nullptr)
            std::rethrow_exception(failed);
    }
}
