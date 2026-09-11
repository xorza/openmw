#pragma once

#include <cstddef>
#include <vector>

namespace Rtx
{
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
}
