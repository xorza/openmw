#pragma once

#include <cassert>
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

    /// A list consumed from the front in the order it was filled, emptied once it is drained and
    /// never before, so what is left in it is never moved.
    template <class T>
    struct Backlog
    {
        std::vector<T> mItems;
        std::size_t mRead = 0;

        std::size_t size() const { return mItems.size() - mRead; }
        bool empty() const { return size() == 0; }
        const T& at(std::size_t offset) const { return mItems[mRead + offset]; }
        void push(const T& item) { mItems.push_back(item); }
        void pop(std::size_t count) { mRead += count; }

        /// Lets go of what was consumed, where everything was.
        void settle()
        {
            if (mRead == mItems.size())
            {
                mItems.clear();
                mRead = 0;
            }
        }
    };

    /// Entries reused across arrivals: as deep as the most one arrival ever wanted, and never
    /// freed. `next` and `keep` are two calls because a caller may not want what it built: a chain
    /// built over a texture that already carried one is left for the next arrival.
    template <class T>
    class Pool
    {
    public:
        /// The entry after the last kept one, made where the pool has never been this deep.
        T& next()
        {
            if (mKept == mEntries.size())
                mEntries.emplace_back();

            return mEntries[mKept];
        }

        /// Says the entry `next` handed out is in use, and gives back its index.
        std::size_t keep()
        {
            assert(mKept < mEntries.size() && "an entry kept that `next` never handed out");
            return mKept++;
        }

        /// Frees every entry, keeping its room.
        void reset() { mKept = 0; }

        T& operator[](const std::size_t at) { return mEntries[at]; }

    private:
        std::vector<T> mEntries;

        /// How many of them a caller has kept, which is where `next` hands out from.
        std::size_t mKept = 0;
    };
}
