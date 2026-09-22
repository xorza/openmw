#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <tuple>
#include <vector>

#include <boost/container/flat_set.hpp>

#include "refusal.hpp"
#include "slots.hpp"

namespace Rtx
{
    /// Content the game uses and this renderer cannot, as one record for every kind of it.
    ///
    /// **One way for all of it.** A reader of content answers with a `Result`, and the error says
    /// why: a mesh's arrays, a texture's format, a sky mesh the archives do not hold. Whatever
    /// decides what becomes of the content reports that here, with the kind and the name. Content
    /// that draws nothing in the game — a lamp of no radius, an empty geometry, a moon of size
    /// nought — draws nothing here too and is not a refusal.
    ///
    /// **Named once and counted.** Each distinct refusal — its kind, its name and its reason — goes
    /// to the log the first time it is met, in one shape at one level, and is counted by kind; a
    /// lamp refused every frame is one refusal. A repeat allocates nothing, so a value refused on
    /// the frame path is reported where it is met. The frame's thread only: what cannot reach it
    /// holds its refusals as `Refusal` and hands them over.
    class Refusals
    {
    public:
        /// Reports `kind` of content named `name`, or unnamed where `name` is empty, left out or
        /// stood in for because of `why`.
        void refuse(Refused kind, std::string_view name, std::string_view why);

        /// Reports what something that could not reach this held.
        void refuse(std::span<const Refusal> held);

        /// How many distinct refusals of `kind` were reported.
        std::uint32_t count(Refused kind) const { return mCounts[static_cast<std::size_t>(kind)]; }

    private:
        /// What one refusal is told apart by: its kind, its name and its reason.
        using Key = std::tuple<Refused, std::string_view, std::string_view>;

        struct KeyOf
        {
            Key operator()(const Refusal& refusal) const { return Key(refusal.mKind, refusal.mName, refusal.mWhy); }
        };

        /// Every distinct refusal met, sorted by its key and searched by one made of views, so a
        /// repeat builds no string.
        boost::container::flat_set<Refusal, KeyedLess<Key, KeyOf>, std::vector<Refusal>> mNamed;
        std::array<std::uint32_t, sRefusedKinds> mCounts{};
    };
}
