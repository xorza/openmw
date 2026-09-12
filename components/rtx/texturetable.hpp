#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <components/vfs/pathutil.hpp>

#include "index.hpp"
#include "slotchanges.hpp"
#include "slotrows.hpp"

namespace Rtx
{
    /// Every texture the scene names, what still names each one, and which slots changed.
    ///
    /// **A slot is reference counted and given back the moment nothing names it.** A texture is
    /// named by the materials that sample it and by the holds a caller takes for something no
    /// material can speak for — an emitter's sprite sheet. Waiting for a sweep instead would keep a
    /// region's images alive across the crossing that left it, which is the memory a route across
    /// the island otherwise settles at everything it visited.
    ///
    /// **Two ways in and one table.** A slot is either a file the content named or a key this
    /// renderer made for something it baked, and never both — `getPaths` and `getBaked` are parallel
    /// and exactly one of them carries a live slot's name. Which of the two is a field and not a
    /// pair of string comparisons. Both are keyed for lookup, so naming the same thing twice is the
    /// same slot.
    ///
    /// **Two arrays and not one, and the empty half is what that costs.** A slot has one name, so
    /// half of the two together stands nothing: tens of kilobytes of a `std::string`'s own inline
    /// bytes over a route, none of it on the heap. One array would have to be `std::string`,
    /// because that is what a baked key is — and every reader of a file's name would then hold a
    /// string this table promises nothing about. `VFS::Path::Normalized` is a type that carries
    /// the guarantee those readers rely on, and the empty half does not buy giving it up.
    ///
    /// **A slot that is freed keeps its index.** The array element it names is written over wherever
    /// it sits, which is what the arrivals list is for, and nothing downstream is renumbered.
    class TextureTable
    {
    public:
        /// The slot for `path`, taking one where this has not met it.
        ///
        /// **The slot is live from here**, before anything names it, and stays live until the last
        /// thing that named it lets go. A caller that adds a texture and then puts it on no material
        /// and takes no hold of it keeps that slot for the rest of the scene, which is a caller
        /// asking for a texture it did not want.
        Index add(VFS::Path::NormalizedView path);

        /// The slot for a texture this renderer made, keyed by `key` rather than by a file, taking
        /// one where `key` is not known.
        ///
        /// **A texture with no file behind it, which the table has to be able to hold.** A composite
        /// baked for a distant terrain chunk is an image nothing can open: the bytes belong to
        /// whatever made it, and what the scene keeps is the slot, because a slot is what a material
        /// points at and what a backend uploads into. Two chunks that would bake the same image must
        /// find the same slot, which is what `key` is for and why it has to be stable across frames.
        ///
        /// The same slots, the same free list and the same reference counting as a file's — this is a
        /// second way in and not a second table. `hold` and `drop` do not care which kind a slot is.
        Index addBaked(std::string_view key);

        /// Takes and gives back one name on a slot. `sNoIndex` is "none" and costs a compare. The
        /// slot is freed by the `drop` after which nothing names it.
        ///
        /// **A particle emitter's sprite, and nothing else so far**, names a texture this way. An
        /// emitter is a placement — it is thrown away and rebuilt every frame — so the texture it
        /// draws with hangs off no material and no table the scene owns; whatever recognises the
        /// emitter between frames is what has to hold it. The alternative is a keep set handed
        /// over on every sweep, which could only be looked at on the frames a mesh or a material
        /// also died.
        void hold(Index texture);
        void drop(Index texture);

        /// Whether nothing stands in `texture`.
        ///
        /// **Read off what the slot is and not off a count**, because a slot is taken before it is
        /// named: a caller that asked the count would find a texture it was in the middle of
        /// building.
        bool isFree(Index texture) const { return mSlots.at(texture) == Kind::Free; }

        /// The file each slot names, empty where it names none.
        std::span<const VFS::Path::Normalized> getPaths() const { return mPaths; }

        /// What made each slot that no file did, parallel to `getPaths`.
        std::span<const std::string> getBaked() const { return mBaked; }

        std::span<const Index> getArrived() const { return mChanges.getArrived(); }
        std::span<const Index> getFreed() const { return mChanges.getFreed(); }

        /// How many slots this has ever taken, which is the share of the scene's structure revision
        /// that textures decide.
        ///
        /// **A revision and not a count of what is here.** A slot freed and taken again has to read
        /// as a change to whoever built from it, and a counter that followed what the table holds
        /// would be back where it started.
        std::uint64_t getRevision() const { return mRevision; }

        void clearArrivals() { mChanges.clearArrivals(); }

    private:
        /// What stands in a slot, and so which of the two name tables carries its name.
        ///
        /// **The row itself, stated rather than deduced.** A slot is a file or a bake and never
        /// both, and asking two names which one it is spells that invariant out at every call
        /// instead of holding it. What else is true of a slot — how many things name it — is the
        /// hold count `SlotRows` keeps beside every row, so an empty name and a count of nought say
        /// the same thing, except in the window between `add` and whatever is about to name what
        /// it returned.
        enum class Kind : std::uint8_t
        {
            Free,

            /// A file the content named. `getPaths` carries it.
            File,

            /// A key this renderer made for something it baked. `getBaked` carries it.
            Baked,
        };

        /// A free slot where there is one, a new row otherwise. The caller names it; this only
        /// finds it somewhere to stand.
        Index takeSlot();

        std::vector<VFS::Path::Normalized> mPaths;
        std::vector<std::string> mBaked;

        /// Parallel to both, one row a slot, and the slots nothing stands in.
        SlotRows<Kind> mSlots;

        SlotChanges mChanges;

        /// Hashes a baked key without building a `std::string` to do it.
        struct BakedHash
        {
            using is_transparent = void;

            std::size_t operator()(std::string_view key) const { return std::hash<std::string_view>{}(key); }
        };

        /// The two lookups, so that naming a texture again is the slot it already has.
        ///
        /// The scan these replace was O(materials x textures). A cell is a hundred of each and would
        /// have paid it on every material it resolved.
        std::unordered_map<VFS::Path::Normalized, Index, VFS::Path::Hash, std::equal_to<>> mPathIndex;
        std::unordered_map<std::string, Index, BakedHash, std::equal_to<>> mBakedIndex;

        std::uint64_t mRevision = 0;
    };
}
