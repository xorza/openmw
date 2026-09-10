#pragma once

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace Rtx
{
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
}
