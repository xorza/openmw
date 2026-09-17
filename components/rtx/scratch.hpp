#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
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

    /// What `Spares` counts on every object it lends: how many hold it, and whether the pool has
    /// it back. The count is the pool's and not the holder's, so one assert says an object was
    /// given back once too often whatever kind of object it is; the flag is what says it was
    /// given back twice, which a list alone would take as two spares.
    struct Lent
    {
        std::uint32_t mLent = 0;
        bool mIsSpare = false;
    };

    /// Objects lent out and given back, and never freed while this stands, so a loader reads a
    /// cell into buffers the last cell grew. Addresses are stable, which is what makes a raw
    /// pointer the right thing to hand another thread. Not thread-safe: one owner, on one thread.
    template <class T>
    requires std::derived_from<T, Lent>
    class Spares
    {
    public:
        /// An object nobody holds, made where none is spare, filled by `fill` before it is handed
        /// out. Where `fill` throws the object is emptied and made spare again first, so no
        /// failure leaves one taken and unfiled — a model whose walk threw was one such per cell
        /// that named it. `T::reuse()` is what a failed fill leaves. Held by nobody until `lend`
        /// says who.
        template <class Fill>
        T& take(Fill fill)
        {
            T& taken = takeSpare();
            try
            {
                fill(taken);
            }
            catch (...)
            {
                taken.reuse();
                give(taken);
                throw;
            }

            return taken;
        }

        /// One more holder of `object`.
        void lend(T& object) { ++object.mLent; }

        /// One holder fewer. @return whether that was the last, so the caller empties the object
        /// before it gives it back.
        bool release(T& object)
        {
            assert(object.mLent > 0 && "an object given back more often than it was lent");
            return --object.mLent == 0;
        }

        /// Gives `object` back for the next `take`. The caller has already emptied it, and nobody
        /// holds it. Allocates nothing.
        void give(T& object)
        {
            assert(object.mLent == 0 && "an object given back while something holds it");
            assert(!object.mIsSpare && "an object given back twice");
            object.mIsSpare = true;
            mSpare.push_back(&object);
        }

        /// How many objects this has made, spare or lent.
        std::size_t size() const { return mAll.size(); }

    private:
        /// A spare off the list, or a new object where the list is empty, unfilled.
        T& takeSpare()
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
            assert(spare.mLent == 0 && "a spare something still holds");
            assert(spare.mIsSpare && "an object on the spare list that give did not put there");
            spare.mIsSpare = false;
            return spare;
        }

        std::vector<std::unique_ptr<T>> mAll;
        std::vector<T*> mSpare;
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

    /// A buffer that empties itself while keeping its room, the way the objects here do. Named,
    /// because MSVC reads a `requires` expression written inline in a generic lambda as false for
    /// every one of them and compiles the `clear` branch against a type that has none.
    template <class Buffer>
    concept Reusable = requires(Buffer& buffer)
    {
        buffer.reuse();
    };

    /// Puts `object` back to its default while keeping the room its buffers grew. Every field not
    /// named is reset, so a new scalar is reset for free and a buffer forgotten reallocates, which
    /// the allocation test sees. A `Lent`'s count and flag are kept: they are the pool's and not
    /// the holder's, and every give-back empties the object first — one that emptied them too
    /// would give an object back twice without the pool noticing.
    template <class T, class... Buffers>
    void reuseKeeping(T& object, Buffers T::*... buffers)
    {
        [[maybe_unused]] const auto empty = []<class Buffer>(Buffer& buffer) {
            if constexpr (Reusable<Buffer>)
                buffer.reuse();
            else
                buffer.clear();
        };

        T fresh;
        (std::swap(fresh.*buffers, object.*buffers), ...);
        (empty(fresh.*buffers), ...);
        if constexpr (std::derived_from<T, Lent>)
            static_cast<Lent&>(fresh) = static_cast<const Lent&>(object);
        object = std::move(fresh);
    }
}
