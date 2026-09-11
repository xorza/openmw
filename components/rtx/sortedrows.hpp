#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <utility>
#include <vector>

namespace Rtx
{
    /// Rows kept in the order of a key taken from each, found by binary search.
    ///
    /// **The key and the order together, because a comparator written at each call is one that can
    /// be written differently.** A search that disagreed with the insertion it is looking for finds
    /// nothing and says nothing at all — which is a silent bug, and the reason this is a type rather
    /// than a `std::lower_bound` at each site.
    ///
    /// **A row's address is not stable**: an insert moves everything above it, so a caller holds the
    /// key rather than a pointer into this. `Spares` is the type for the other case.
    ///
    /// **Not a map.** What these hold is walked in order every frame, which is the case a sorted
    /// vector is for: one allocation, a handful of rows to a cache line, and a walk that is the same
    /// walk on every machine — which is what makes the slot a placement takes a fact about the
    /// world.
    ///
    /// @tparam KeyOf what a row's key is, as a callable taking the row. Stateless, because the order
    ///         belongs to the type rather than to the instance.
    template <class Row, class Key, class KeyOf>
    class SortedRows
    {
    public:
        using Rows = std::vector<Row>;
        using iterator = typename Rows::iterator;
        using const_iterator = typename Rows::const_iterator;

        /// The row `key` names, or null where nothing holds it.
        const Row* find(const Key& key) const
        {
            const const_iterator at = lowerBound(key);
            return at != mRows.end() && !(key < KeyOf{}(*at)) ? &*at : nullptr;
        }

        Row* find(const Key& key) { return const_cast<Row*>(std::as_const(*this).find(key)); }

        bool contains(const Key& key) const { return find(key) != nullptr; }

        /// The row `key` names, for a caller whose contract is that there is one.
        ///
        /// **A reference, because `find` hands a caller a case it never handles.** Three callers
        /// asserted the pointer and dereferenced it, and `NDEBUG` takes the assert away — which
        /// left a release build with a path that reads through nought and a compiler right to say
        /// so. A reference cannot be nought, so the case stops existing rather than being
        /// suppressed.
        ///
        /// **The assert is the contract and not a guard.** A key that is not here is a caller that
        /// lost track of what it holds, which is a logic error and never a state to handle.
        const Row& at(const Key& key) const
        {
            const const_iterator found = lowerBound(key);
            assert(found != mRows.end() && !(key < KeyOf{}(*found)) && "a key read that nothing holds");

            return *found;
        }

        Row& at(const Key& key) { return const_cast<Row&>(std::as_const(*this).at(key)); }

        /// The row `key` names, inserting what `make` answers where nothing holds it.
        ///
        /// **One search either way**, which is what makes this one call rather than a `find` and an
        /// `insert`: a cell arriving asks it once per model it names.
        template <class Make>
        Row& findOrInsert(const Key& key, Make make)
        {
            const iterator at = lowerBound(key);
            if (at != mRows.end() && !(key < KeyOf{}(*at)))
                return *at;

            return *mRows.insert(at, make());
        }

        /// Puts `row` where its own key says. Asserts where something already holds that key.
        Row& insert(Row row)
        {
            const iterator at = lowerBound(KeyOf{}(row));
            assert((at == mRows.end() || KeyOf{}(row) < KeyOf{}(*at)) && "a key inserted twice");

            return *mRows.insert(at, std::move(row));
        }

        /// Takes the row `key` names away and answers it. Asserts where nothing holds it.
        Row take(const Key& key)
        {
            const iterator at = lowerBound(key);
            assert(at != mRows.end() && !(key < KeyOf{}(*at)) && "a key taken that nothing holds");

            Row taken = std::move(*at);
            mRows.erase(at);
            return taken;
        }

        /// Drops the row `key` names. Asserts where nothing holds it.
        void erase(const Key& key)
        {
            const iterator at = lowerBound(key);
            assert(at != mRows.end() && !(key < KeyOf{}(*at)) && "a key dropped that nothing holds");

            mRows.erase(at);
        }

        /// Drops the row at `at` and answers what follows it, for a sweep that walks them all.
        iterator erase(const_iterator at) { return mRows.erase(at); }

        template <class Gone>
        void eraseIf(Gone gone)
        {
            std::erase_if(mRows, gone);
        }

        void clear() { mRows.clear(); }

        std::size_t size() const { return mRows.size(); }

        iterator begin() { return mRows.begin(); }
        iterator end() { return mRows.end(); }

    private:
        /// The first row whose key is not below `key`, which is where one goes and where one is.
        /// **The one comparator this type has**, which is the whole of what it is for.
        iterator lowerBound(const Key& key) { return std::lower_bound(mRows.begin(), mRows.end(), key, before); }
        const_iterator lowerBound(const Key& key) const
        {
            return std::lower_bound(mRows.begin(), mRows.end(), key, before);
        }

        static bool before(const Row& row, const Key& wanted) { return KeyOf{}(row) < wanted; }

        Rows mRows;
    };
}
