#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include <osg/ref_ptr>

#include "index.hpp"

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
    /// The epoch is what `retire` sweeps on: a walk stamps everything it resolves, so anything
    /// still carrying an older stamp is something the graph no longer has.
    struct Known
    {
        Index mIndex = sNoIndex;
        std::uint64_t mEpoch = 0;
    };

    /// What the scene knows one `osg` object as, keyed so the object cannot go while the entry
    /// stands. See `ByAddress`.
    template <class T, class Held = Known>
    using Identity = std::unordered_map<osg::ref_ptr<T>, Held, ByAddress<T>, ByAddress<T>>;

    /// Drops every entry not stamped with `epoch`, and collects what is left.
    ///
    /// The survivors go out unsorted. `SceneDesc::release` takes them that way.
    template <class Map>
    std::uint32_t sweep(Map& known, std::uint64_t epoch, std::vector<Index>& live)
    {
        live.clear();
        live.reserve(known.size());

        std::uint32_t dropped = 0;
        for (auto entry = known.begin(); entry != known.end();)
        {
            if (entry->second.mEpoch == epoch)
            {
                live.push_back(entry->second.mIndex);
                ++entry;
                continue;
            }

            entry = known.erase(entry);
            ++dropped;
        }

        return dropped;
    }
}
