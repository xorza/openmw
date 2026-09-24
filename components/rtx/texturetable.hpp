#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

#include <components/vfs/pathutil.hpp>

#include "runs.hpp"
#include "shaders/scene.h"
#include "slots.hpp"
#include "textureencoding.hpp"
#include "texturewrap.hpp"

namespace Rtx
{
    /// What stands in a texture slot, and so which of the row's two names carries it, stated
    /// rather than deduced from two names.
    enum class TextureKind : std::uint8_t
    {
        Free,

        /// A file the content named. `TextureRow::mPath` carries it.
        File,

        /// A key this renderer made for something it baked. `TextureRow::mBaked` carries it.
        Baked,
    };

    /// One slot of the table: what stands in it, its name under the kind, and how it is addressed
    /// past its edges. The two names are two fields because a file's name is a
    /// `VFS::Path::Normalized` with a guarantee its readers rely on.
    struct TextureRow
    {
        TextureKind mKind = TextureKind::Free;
        VFS::Path::Normalized mPath;
        std::string mBaked;
        TextureWrap mWrap = TextureWrap::Repeat;

        /// What the slot is read as. A bake is a colour.
        TextureEncoding mEncoding = TextureEncoding::Colour;
    };

    /// Every texture the scene names, what still names each one, and which slots changed. A slot
    /// is reference counted and given back the moment nothing names it, because waiting for a
    /// sweep would keep a region's images alive across the crossing that left it. Two ways in and
    /// one table: a slot is a file the content named or a key this renderer made for something it
    /// baked, never both. A slot that is freed keeps its index.
    ///
    /// **A slot is a file, its wrap and its encoding.** The same file bound clamped and bound
    /// repeating is two slots, because a sampler is per slot and the wrap is the sampler's; the
    /// same file bound as a colour and as data is two, because the encoding is the image's format.
    /// A file names up to eight, one per `TextureWrap` and `TextureEncoding`.
    class TextureTable
    {
    public:
        /// How many slots stand at once: the bindless array's, less the one its neutral texel
        /// takes. A world past it is content this renderer draws neutral, and never a frame that
        /// stops: an arrival that would take one more slot is refused and counted.
        static constexpr std::size_t sCapacity = Shaders::TEXTURE_NEUTRAL;

        /// The slot for `path` under `wrap` and `encoding`, taking one where this has not met the
        /// three. Live from here, before anything names it, and until the last thing that named it
        /// lets go. `sNoIndex` where they are new and `sCapacity` slots already stand.
        Index add(VFS::Path::NormalizedView path, TextureWrap wrap = TextureWrap::Repeat,
            TextureEncoding encoding = TextureEncoding::Colour);

        /// The slot for a texture this renderer made — a composite baked for a distant chunk —
        /// keyed by `key` rather than by a file, taking one where `key` is not known. Two chunks
        /// that would bake the same image must find the same slot, so `key` has to be stable across
        /// frames. The same slots and the same reference counting as a file's. Clamped, because a
        /// bake is one image whose coordinates run edge to edge. `sNoIndex` as `add` answers it.
        Index addBaked(std::string_view key);

        /// The slot `path` stands in as a colour under any wrap, or `sNoIndex` where it stands in
        /// none. What a bake made from a file's alpha finds its source by: the alpha is the same
        /// under every wrap, and the bake's key carries the file and not the wrap.
        Index findFile(VFS::Path::NormalizedView path) const;

        /// Takes and gives back one name on a slot. A slot this never hands out — `sNoIndex`, and
        /// the neutral texel a layer names where the table had no room — is "none" and costs a
        /// compare. The slot is freed by the `drop` after which nothing names it. A particle
        /// emitter's sprite names a texture this way: an emitter is rebuilt every frame, so
        /// whatever recognises it between frames is what has to hold the texture.
        void hold(Index texture);
        void drop(Index texture);

        /// Whether nothing stands in `texture`. Read off what the slot is and not off a count,
        /// because a slot is taken before it is named.
        bool isFree(Index texture) const { return mRows.at(texture).mKind == TextureKind::Free; }

        /// How many slots stand a texture, which is what an empty scene has nought of.
        std::size_t getLiveCount() const { return mRows.getLiveCount(); }

        /// What names `texture`, which is nought only between the take and the hold that follows
        /// it: a live slot with no hold after that is one no sweep can reach.
        std::uint32_t getHolds(Index texture) const { return mRows.getHolds(texture); }

        /// Every slot, free ones included, in slot order.
        std::span<const TextureRow> getRows() const { return mRows.getRows(); }

        std::span<const Index> getArrived() const { return mChanges.getArrived(); }
        std::span<const Index> getFreed() const { return mChanges.getFreed(); }

        /// How many slots this has ever taken, which is the share of the scene's structure revision
        /// that textures decide. A revision and not a count, because a slot freed and taken again
        /// has to read as a change.
        std::uint64_t getRevision() const { return mRevision; }

        void clearArrivals() { mChanges.clearArrivals(); }

        /// How many new textures were refused because `sCapacity` slots stood, ever. Drawn neutral,
        /// and reported by `SceneTextures`.
        std::uint32_t getRefused() const { return mRefused; }

    private:
        /// Whether a new slot may be taken, counting and reporting the refusal where it may not.
        bool hasRoom();

        /// Puts `row` in a free slot where there is one, in a new one otherwise, and counts the
        /// arrival.
        Index takeSlot(TextureRow row);

        /// The slot a file holds under each encoding and wrap, `sNoIndex` where it holds none.
        using FileSlots = std::array<std::array<Index, sTextureWrapCount>, sTextureEncodingCount>;

        SlotRows<TextureRow> mRows;

        SlotChanges mChanges;

        /// Hashes a baked key without building a `std::string` to do it.
        struct BakedHash
        {
            using is_transparent = void;

            std::size_t operator()(std::string_view key) const { return std::hash<std::string_view>{}(key); }
        };

        /// The two lookups, so that naming a texture again is the slot it already has, where a scan
        /// was O(materials x textures): a cell is a hundred of each and paid it on every material
        /// it resolved. A file's entry leaves the first map when its last slot is freed.
        std::unordered_map<VFS::Path::Normalized, FileSlots, VFS::Path::Hash, std::equal_to<>> mPathIndex;
        std::unordered_map<std::string, Index, BakedHash, std::equal_to<>> mBakedIndex;

        std::uint64_t mRevision = 0;
        std::uint32_t mRefused = 0;
    };
}
