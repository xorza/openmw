#pragma once

#include <cassert>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace Rtx
{
    /// Values given back whole and taken again, so that what a row's vectors grew is room the next
    /// row refills.
    ///
    /// **For a row held by value in a table that moves it**, which `Spares` cannot be: that one
    /// hands out stable addresses and keeps every object it ever made, where a `SortedRows` row is
    /// moved into and out of its table and is nobody's address. What is kept here is the row's
    /// buffers and not the row — a `take` is a moved-from value the caller fills, and a `give` is a
    /// value the caller has emptied.
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

    /// Objects lent out and given back, and never freed while this stands.
    ///
    /// **What lets a loader read a cell into buffers the last cell grew.** An object given back
    /// keeps whatever room its vectors reached, so the next `take` refills that room rather than
    /// going to the heap for it — which is the rule every loader in this renderer keeps, and the
    /// one a reader on its own thread would otherwise break once per cell and once per model.
    ///
    /// **Addresses are stable.** Each object lives where it was made, so a pointer handed to
    /// another thread stays good until the object is given back — which is what makes a raw
    /// pointer, and not a shared one, the right thing to hand over.
    ///
    /// Not thread-safe: one owner, on one thread.
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

    /// Puts `object` back to its default while keeping the room its buffers grew.
    ///
    /// **Every field not named here is reset, and only a buffer needs naming.** A `reuse()` written
    /// as a list of fields resets a new field only if its author remembers; this one resets a new
    /// scalar for free, and a buffer forgotten is a buffer that reallocates, which the allocation
    /// test sees.
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

    /// A list consumed from the front in the order it was filled.
    ///
    /// **Emptied once it is drained and never before**, so a route allocates for it only while it
    /// grows, and what is left in it is never moved. Beside `Pool` and `Spares` because it is the
    /// same kind of thing: a container with one rule about when its room goes back.
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

    /// Entries reused across arrivals: as deep as the most one arrival ever wanted, and never freed.
    ///
    /// **Refilled rather than emptied**, because what an entry holds is room the heap gave it. An
    /// entry handed out again writes into whatever the last one grew, so an arrival pays for that
    /// room once for the life of the run rather than on the frame a cell lands.
    ///
    /// **`next` and `keep` are two calls because a caller may not want what it built.** A chain
    /// built over a texture that already carried one is empty and is left for the next arrival, so
    /// what says an entry is in use is the caller and not the handing out.
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
