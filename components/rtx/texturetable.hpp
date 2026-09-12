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

#include "runs.hpp"
#include "slots.hpp"

namespace Rtx
{
    /// Every texture the scene names, what still names each one, and which slots changed. A slot
    /// is reference counted and given back the moment nothing names it, because waiting for a
    /// sweep would keep a region's images alive across the crossing that left it. Two ways in and
    /// one table: a slot is a file the content named or a key this renderer made for something it
    /// baked, never both, in two parallel arrays because a file's name is a
    /// `VFS::Path::Normalized` with a guarantee its readers rely on. A slot that is freed keeps
    /// its index.
    class TextureTable
    {
    public:
        /// The slot for `path`, taking one where this has not met it. Live from here, before
        /// anything names it, and until the last thing that named it lets go.
        Index add(VFS::Path::NormalizedView path);

        /// The slot for a texture this renderer made — a composite baked for a distant chunk —
        /// keyed by `key` rather than by a file, taking one where `key` is not known. Two chunks
        /// that would bake the same image must find the same slot, so `key` has to be stable across
        /// frames. The same slots and the same reference counting as a file's.
        Index addBaked(std::string_view key);

        /// Takes and gives back one name on a slot. `sNoIndex` is "none" and costs a compare. The
        /// slot is freed by the `drop` after which nothing names it. A particle emitter's sprite
        /// names a texture this way: an emitter is rebuilt every frame, so whatever recognises it
        /// between frames is what has to hold the texture.
        void hold(Index texture);
        void drop(Index texture);

        /// Whether nothing stands in `texture`. Read off what the slot is and not off a count,
        /// because a slot is taken before it is named.
        bool isFree(Index texture) const { return mSlots.at(texture) == Kind::Free; }

        /// The file each slot names, empty where it names none.
        std::span<const VFS::Path::Normalized> getPaths() const { return mPaths; }

        /// What made each slot that no file did, parallel to `getPaths`.
        std::span<const std::string> getBaked() const { return mBaked; }

        std::span<const Index> getArrived() const { return mChanges.getArrived(); }
        std::span<const Index> getFreed() const { return mChanges.getFreed(); }

        /// How many slots this has ever taken, which is the share of the scene's structure revision
        /// that textures decide. A revision and not a count, because a slot freed and taken again
        /// has to read as a change.
        std::uint64_t getRevision() const { return mRevision; }

        void clearArrivals() { mChanges.clearArrivals(); }

    private:
        /// What stands in a slot, and so which of the two name tables carries its name, stated
        /// rather than deduced from two names.
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

        /// The two lookups, so that naming a texture again is the slot it already has, where a scan
        /// was O(materials x textures): a cell is a hundred of each and paid it on every material
        /// it resolved.
        std::unordered_map<VFS::Path::Normalized, Index, VFS::Path::Hash, std::equal_to<>> mPathIndex;
        std::unordered_map<std::string, Index, BakedHash, std::equal_to<>> mBakedIndex;

        std::uint64_t mRevision = 0;
    };
}
