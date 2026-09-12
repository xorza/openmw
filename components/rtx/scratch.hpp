#pragma once

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace Rtx
{
    /// Values given back whole and taken again, so that what a row's vectors grew is room the next
    /// row refills — for a row held by value in a table that moves it, where `Spares` hands out
    /// stable addresses.
    template <class T>
    class Recycled
    {
    public:
        /// A value nobody holds, empty and carrying whatever room its last holder grew — or a fresh
        /// one where none is spare.
        T take()
        {
            if (mSpare.empty())
                return T{};

            T taken = std::move(mSpare.back());
            mSpare.pop_back();
            return taken;
        }

        /// Gives `value` back for the next `take`. The caller has already emptied it.
        void give(T&& value) { mSpare.push_back(std::move(value)); }

    private:
        std::vector<T> mSpare;
    };

    /// Objects lent out and given back, and never freed while this stands, so a loader reads a
    /// cell into buffers the last cell grew. Addresses are stable, which is what makes a raw
    /// pointer the right thing to hand another thread. Not thread-safe: one owner, on one thread.
    template <class T>
    class Spares
    {
    public:
        /// An object nobody holds, made where none is spare.
        T& take()
        {
            if (mSpare.empty())
            {
                mAll.push_back(std::make_unique<T>());

                // Room for every object to be spare at once, so that a give-back never reaches the
                // heap: the thread gives back while the frame counts what a walk allocated.
                mSpare.reserve(mAll.size());
                return *mAll.back();
            }

            T& spare = *mSpare.back();
            mSpare.pop_back();
            return spare;
        }

        /// Gives `object` back for the next `take`. The caller has already emptied it. Allocates
        /// nothing.
        void give(T& object) { mSpare.push_back(&object); }

        /// How many objects this has made, spare or lent.
        std::size_t size() const { return mAll.size(); }

    private:
        std::vector<std::unique_ptr<T>> mAll;
        std::vector<T*> mSpare;
    };

    /// Puts `object` back to its default while keeping the room its buffers grew. Every field not
    /// named is reset, so a new scalar is reset for free and a buffer forgotten reallocates, which
    /// the allocation test sees.
    template <class T, class... Buffers>
    void reuseKeeping(T& object, Buffers T::*... buffers)
    {
        const auto empty = [](auto& buffer) {
            if constexpr (requires { buffer.reuse(); })
                buffer.reuse();
            else
                buffer.clear();
        };

        T fresh;
        (std::swap(fresh.*buffers, object.*buffers), ...);
        (empty(fresh.*buffers), ...);
        object = std::move(fresh);
    }
}
