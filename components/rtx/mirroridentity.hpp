#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

#include <osg/ref_ptr>

#include "index.hpp"
#include "mirrorpass.hpp"

namespace Rtx
{
    /// Hashes and compares an owning key by the address it holds.
    ///
    /// **What lets an identity map hold its subject alive without paying for that on a lookup.** A
    /// map keyed on a raw `osg` pointer can be fooled: the engine frees a body part and the
    /// allocator puts the replacement exactly where it was, so the walk that meets the new one finds
    /// the old one's entry and mirrors geometry it has nothing to do with. A `ref_ptr` key makes the
    /// address *true* — nothing else can hold it while the entry does — and being transparent is
    /// what keeps every lookup from a raw pointer out of the reference count.
    template <class T>
    struct ByAddress
    {
        using is_transparent = void;

        std::size_t operator()(const osg::ref_ptr<T>& value) const { return std::hash<const T*>{}(value.get()); }
        std::size_t operator()(const T* value) const { return std::hash<const T*>{}(value); }

        bool operator()(const osg::ref_ptr<T>& left, const osg::ref_ptr<T>& right) const
        {
            return left.get() == right.get();
        }
        bool operator()(const osg::ref_ptr<T>& left, const T* right) const { return left.get() == right; }
        bool operator()(const T* left, const osg::ref_ptr<T>& right) const { return left == right.get(); }
    };

    /// An entry in one of the identity maps, and when it was last met.
    ///
    /// The epoch is what a sweep runs on: a walk stamps everything it resolves, so anything still
    /// carrying an older stamp is something the graph no longer has.
    struct Known
    {
        Index mIndex = sNoIndex;
        std::uint64_t mEpoch = 0;
    };

    /// A map of what the mirror knows, and how much of it the walk in progress has reached.
    ///
    /// **The count is what lets a sweep be skipped rather than run to find nothing.** A world that
    /// stands still reaches every entry it holds, and a map that was reached whole has nothing stale
    /// in it — where the sweep would visit tens of thousands of entries, a cache miss apiece, to
    /// reach the same conclusion. The count is only ever an equality: a walk stamps an entry once,
    /// so it cannot pass the size, and anything short of it means something went unreached.
    ///
    /// **The count belongs to one epoch, and the table remembers which.** A table is not always
    /// retired — where nothing died anywhere, the mesh table and the material table are left alone —
    /// so a reset written into the sweep would leave one epoch's count standing over the next
    /// epoch's walk.
    ///
    /// **Every write goes through this class**, because a count kept beside the map is a count free
    /// to fall behind it. `stamp`, `add`, `reach` and `abandon` are the whole of what a walk does to
    /// one, and each keeps the count true.
    template <class Map>
    class Kept
    {
    public:
        using Entry = typename Map::iterator;

        /// What `reach` found: the entry, stamped, and whether the call is what put it there.
        struct Arrival
        {
            Entry mEntry;
            bool mArrived;
        };

        /// @param pass the walk in progress, borrowed: the epoch it stamps with is the mirror's own
        ///        rather than a copy free to fall behind it.
        explicit Kept(const MirrorPass& pass)
            : mPass(pass)
        {
        }

        Entry end() { return mKnown.end(); }

        /// The entry for `key`, or `end()`. **Unstamped**: `stamp` is what says the walk met it.
        template <class Key>
        Entry find(const Key& key)
        {
            return mKnown.find(key);
        }

        /// Records that the walk in progress met `entry`.
        ///
        /// Counted on the way to the stamp rather than by the stamp, so an entry two walks of one
        /// epoch both reach counts once.
        void stamp(Entry entry) { stamp(entry->second); }

        /// The same for an entry a caller already holds, which is what a replay of a paged chunk
        /// stamps with: it looks every entry of a run up before it stamps any of them, so that a
        /// run it turns out not to hold is a walk rather than a half-stamped chunk.
        void stamp(Known& held)
        {
            freshen();
            mReached += held.mEpoch != mPass.mEpoch ? 1 : 0;
            held.mEpoch = mPass.mEpoch;
        }

        /// Adds what the walk has just resolved, stamped. `key` must not already be held.
        template <class Key, class Held>
        void add(const Key& key, Held held)
        {
            freshen();
            held.mEpoch = mPass.mEpoch;

            [[maybe_unused]] const bool arrived = mKnown.emplace(key, std::move(held)).second;
            assert(arrived && "an identity the map already held, added again");

            ++mReached;
        }

        /// The entry for `key`, stamped, made where the map holds none.
        ///
        /// A made entry arrives with its fields default and the caller fills them. **The first
        /// epoch is why the arrival is counted rather than deduced**: an epoch of nought is what a
        /// default entry carries and what the first walk stamps with, so a stamp alone cannot tell
        /// the two apart.
        template <class Key>
        Arrival reach(const Key& key)
        {
            freshen();

            const auto [entry, arrived] = mKnown.try_emplace(key);
            mReached += arrived || entry->second.mEpoch != mPass.mEpoch ? 1 : 0;
            entry->second.mEpoch = mPass.mEpoch;

            return Arrival{ .mEntry = entry, .mArrived = arrived };
        }

        /// Lets go of `entry` in the middle of a walk, where what it held turned out to describe
        /// something else.
        ///
        /// **Why the count alone cannot say a map is whole.** The slot the entry named is named by
        /// nothing now, so the frame owes a sweep however much of the map it went on to reach — a
        /// replacement stamped in its place brings the count back up to the size and hides it.
        void abandon(Entry entry)
        {
            freshen();
            mReached -= entry->second.mEpoch == mPass.mEpoch ? 1 : 0;
            mKnown.erase(entry);
            mAbandoned = true;
        }

        /// Whether the walk in progress met every entry the map holds and abandoned none — so a
        /// sweep would erase nothing, and every slot the map names still stands.
        bool whole() const
        {
            // A count from an earlier epoch says nothing about this one, which has reached nothing
            // yet: only an empty map is whole then.
            if (mCountedEpoch != mPass.mEpoch)
                return mKnown.empty();

            return !mAbandoned && mReached == mKnown.size();
        }

        /// Drops every entry the epoch did not reach, and collects the slots the survivors name.
        ///
        /// **Not skipped where the map is whole, unlike `retire`.** The list it fills is read beside
        /// another table's, so a caller that wants either wants both of this epoch — `whole` is what
        /// it asks first. The survivors go out unsorted, which is how `SceneDesc::release` takes
        /// them.
        ///
        /// @return how many were dropped.
        std::uint32_t sweep(std::vector<Index>& live)
        {
            live.clear();
            live.reserve(mKnown.size());

            std::uint32_t dropped = 0;
            for (auto entry = mKnown.begin(); entry != mKnown.end();)
            {
                if (entry->second.mEpoch == mPass.mEpoch)
                {
                    live.push_back(entry->second.mIndex);
                    ++entry;
                    continue;
                }

                entry = mKnown.erase(entry);
                ++dropped;
            }

            settle();
            return dropped;
        }

        /// Drops every entry the epoch did not reach, handing `drop` what each held on its way out.
        ///
        /// **Skipped where the map is whole**, which is the point of the count.
        template <class Drop>
        void retire(Drop drop)
        {
            if (whole())
                return;

            std::erase_if(mKnown, [this, &drop](const auto& entry) {
                if (entry.second.mEpoch == mPass.mEpoch)
                    return false;

                drop(entry.second);
                return true;
            });

            settle();
        }

        /// The same where the entry holds nothing to give back.
        void retire()
        {
            retire([](const auto&) {});
        }

    private:
        /// Starts the count again where the epoch has moved on since it was last touched.
        void freshen()
        {
            if (mCountedEpoch == mPass.mEpoch)
                return;

            mCountedEpoch = mPass.mEpoch;
            mReached = 0;
            mAbandoned = false;
        }

        /// What a sweep leaves behind: every entry still here carries this epoch's stamp, and every
        /// slot the map names stands.
        void settle()
        {
            mCountedEpoch = mPass.mEpoch;
            mReached = mKnown.size();
            mAbandoned = false;
        }

        Map mKnown;
        const MirrorPass& mPass;

        std::uint64_t mCountedEpoch = 0;
        std::size_t mReached = 0;
        bool mAbandoned = false;
    };

    /// What the scene knows one `osg` object as, keyed so the object cannot go while the entry
    /// stands. See `ByAddress`.
    template <class T, class Held = Known>
    using Identity = Kept<std::unordered_map<osg::ref_ptr<T>, Held, ByAddress<T>, ByAddress<T>>>;
}
