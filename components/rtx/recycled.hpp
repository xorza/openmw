#pragma once

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
}
