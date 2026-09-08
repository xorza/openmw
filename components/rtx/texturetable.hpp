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
    /// **A slot that is freed keeps its index.** The array element it names is written over wherever
    /// it sits, which is what the arrivals list is for, and nothing downstream is renumbered.
    class TextureTable
    {
    public:
        /// The slot for `path`, taking one where this has not met it.
        Index add(VFS::Path::NormalizedView path);

        /// The slot for a texture this renderer made, keyed by `key` rather than by a file.
        Index addBaked(std::string_view key);

        /// Takes and gives back one name on a slot. `sNoIndex` is "none" and costs a compare.
        void hold(Index texture);
        void drop(Index texture);

        /// Whether nothing stands in `texture`.
        ///
        /// **Read off what the slot is and not off a count**, because a slot is taken before it is
        /// named: a caller that asked the count would find a texture it was in the middle of
        /// building.
        bool isFree(Index texture) const { return mSlots[texture].mKind == Kind::Free; }

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
        enum class Kind : std::uint8_t
        {
            Free,

            /// A file the content named. `getPaths` carries it.
            File,

            /// A key this renderer made for something it baked. `getBaked` carries it.
            Baked,
        };

        /// Everything true of a slot that is not its name.
        ///
        /// **One row and not two arrays**, because the two are written by the same three calls: a
        /// slot is taken and named together, and given back together.
        struct Slot
        {
            /// How many things name it.
            ///
            /// **Kept beside the name rather than read off it.** A slot's name is cleared when the
            /// last reference goes and the slot goes onto the free list — so an empty name and a
            /// count of nought say the same thing, except in the window between `add` and whatever
            /// is about to name what it returned.
            std::uint32_t mRefs = 0;

            /// **Which of the two named it, stated rather than deduced.** A slot is a file or a
            /// bake and never both, and asking two names which one it is spells that invariant out
            /// at every call instead of holding it.
            Kind mKind = Kind::Free;
        };

        /// A free slot where there is one, a new row otherwise. The caller names it; this only
        /// finds it somewhere to stand.
        Index takeSlot();

        std::vector<VFS::Path::Normalized> mPaths;
        std::vector<std::string> mBaked;

        /// Parallel to both, one row a slot.
        std::vector<Slot> mSlots;

        /// A min-heap. `Rtx::takeFreeSlot` says why the lowest.
        std::vector<Index> mFree;

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
