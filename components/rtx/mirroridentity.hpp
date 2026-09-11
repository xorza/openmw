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

    /// An entry in one of the identity maps, when it was last met, and what holds it.
    ///
    /// The epoch is what a sweep runs on: a walk stamps everything it resolves, so anything still
    /// carrying an older stamp is something the graph no longer has.
    ///
    /// **The holds are the other keeper.** A residency stands rows the walk never meets — a
    /// distant cell's models — under the same entries the walk would find a clone's mesh under, so
    /// that a mesh both stand is one mesh. A count on the entry, taken with the hold, is what the
    /// sweep keeps by whatever the stamp says.
    struct Known
    {
        Index mIndex = sNoIndex;
        std::uint64_t mEpoch = 0;
        std::uint32_t mHolds = 0;
    };

    /// A map of what the mirror knows, and how much of it the walk in progress has reached.
    ///
    /// **The count is what lets a sweep be skipped rather than run to find nothing.** A world that
    /// stands still reaches every entry it holds, and a map that was reached whole has nothing stale
    /// in it — where the sweep would visit tens of thousands of entries, a cache miss apiece, to
    /// reach the same conclusion. The count is only ever an equality: a walk stamps an entry once,
    /// so it cannot pass the size, and anything short of it means something went unreached.
    ///
    /// **A held entry counts as reached without being stamped.** `mHeld` is how many entries carry
    /// a hold, and a stamp on one of them is not counted, so the two counts together are the size
    /// exactly when every unheld entry was met — which is what `whole` asks.
    ///
    /// **The count belongs to one epoch, and the table remembers which.** A table is not always
    /// retired — where nothing died anywhere, the mesh table and the material table are left alone —
    /// so a reset written into the sweep would leave one epoch's count standing over the next
    /// epoch's walk.
    ///
    /// **Every write goes through this class**, because a count kept beside the map is a count free
    /// to fall behind it. `stamp`, `add`, `reach`, `hold`, `drop` and `abandon` are the whole of what
    /// a walk and a residency do to one, and each keeps the counts true.
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

        /// Room for `count` entries before the table rehashes. A rehash on the frame a cell arrives
        /// is what this is called once to prevent.
        void reserve(std::size_t count) { mKnown.reserve(count); }

        /// The entry for `key`, or `end()`. **Unstamped**: `stamp` is what says the walk met it.
        template <class Key>
        Entry find(const Key& key)
        {
            return mKnown.find(key);
        }

        /// Records that the walk in progress met `entry`.
        ///
        /// Counted on the way to the stamp rather than by the stamp, so an entry two walks of one
        /// epoch both reach counts once. A held entry is stamped and not counted: `mHeld` already
        /// stands for it.
        void stamp(Entry entry) { stamp(entry->second); }

        /// The same for an entry a caller already holds.
        void stamp(Known& held)
        {
            freshen();
            mReached += held.mHolds == 0 && held.mEpoch != mPass.mEpoch ? 1 : 0;
            held.mEpoch = mPass.mEpoch;
        }

        /// Takes one hold on `entry`, which keeps it — and the row it names — through every sweep
        /// until the hold is given back.
        void hold(Entry entry)
        {
            freshen();

            Known& held = entry->second;
            if (held.mHolds++ != 0)
                return;

            ++mHeld;
            mReached -= held.mEpoch == mPass.mEpoch ? 1 : 0;
        }

        /// Gives one hold back. An entry no hold and no stamp keeps is the next sweep's, and the
        /// sweep is owed for it whatever else the walk reached.
        void drop(Entry entry)
        {
            freshen();

            Known& held = entry->second;
            assert(held.mHolds > 0 && "an entry given back more often than it was held");
            if (--held.mHolds != 0)
                return;

            --mHeld;
            if (held.mEpoch == mPass.mEpoch)
                ++mReached;
            else
                mAbandoned = true;
        }

        /// Adds what the walk has just resolved, stamped, and hands the entry back. `key` must not
        /// already be held.
        template <class Key, class Held>
        Entry add(const Key& key, Held held)
        {
            freshen();
            held.mEpoch = mPass.mEpoch;
            held.mHolds = 0;

            const auto [entry, arrived] = mKnown.emplace(key, std::move(held));
            assert(arrived && "an identity the map already held, added again");

            ++mReached;
            return entry;
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
            Known& held = entry->second;
            mReached += held.mHolds == 0 && (arrived || held.mEpoch != mPass.mEpoch) ? 1 : 0;
            held.mEpoch = mPass.mEpoch;

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

            const Known& held = entry->second;
            if (held.mHolds != 0)
                --mHeld;
            else
                mReached -= held.mEpoch == mPass.mEpoch ? 1 : 0;

            mKnown.erase(entry);
            mAbandoned = true;
        }

        /// Whether the walk in progress met every entry the map holds and abandoned none — so a
        /// sweep would erase nothing, and every slot the map names still stands.
        bool whole() const
        {
            // A count from an earlier epoch says nothing about this one, which has reached nothing
            // yet: only a map with nothing unheld in it is whole then.
            if (mCountedEpoch != mPass.mEpoch)
                return mKnown.size() == mHeld;

            return !mAbandoned && mReached + mHeld == mKnown.size();
        }

        /// Drops every entry neither the epoch nor a hold keeps, and collects the slots the
        /// survivors name.
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
                if (keeps(entry->second))
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

        /// Drops every entry neither the epoch nor a hold keeps, handing `drop` what each held on
        /// its way out.
        ///
        /// **Skipped where the map is whole**, which is the point of the count.
        template <class Drop>
        void retire(Drop drop)
        {
            if (whole())
                return;

            std::erase_if(mKnown, [this, &drop](const auto& entry) {
                if (keeps(entry.second))
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
        /// Whether a sweep keeps `held`: met this epoch, or held by something.
        bool keeps(const Known& held) const { return held.mHolds != 0 || held.mEpoch == mPass.mEpoch; }

        /// Starts the count again where the epoch has moved on since it was last touched.
        void freshen()
        {
            if (mCountedEpoch == mPass.mEpoch)
                return;

            mCountedEpoch = mPass.mEpoch;
            mReached = 0;
            mAbandoned = false;
        }

        /// What a sweep leaves behind: every entry still here carries this epoch's stamp or a hold,
        /// and every slot the map names stands.
        void settle()
        {
            mCountedEpoch = mPass.mEpoch;
            mReached = mKnown.size() - mHeld;
            mAbandoned = false;
        }

        Map mKnown;
        const MirrorPass& mPass;

        std::uint64_t mCountedEpoch = 0;
        std::size_t mReached = 0;

        /// How many entries carry a hold. Kept across epochs, unlike `mReached`: a hold is not a
        /// fact about a walk.
        std::size_t mHeld = 0;

        bool mAbandoned = false;
    };

    /// What the scene knows one `osg` object as, keyed so the object cannot go while the entry
    /// stands. See `ByAddress`.
    template <class T, class Held = Known>
    using Identity = Kept<std::unordered_map<osg::ref_ptr<T>, Held, ByAddress<T>, ByAddress<T>>>;
}
