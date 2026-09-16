#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Rtx
{
    class SceneDesc;

    /// One thing a scene holds, and one column of a hashes file: a digest each rather than one for
    /// the lot, because a run whose layout differs has to say which table moved. In the order the
    /// tables are laid out.
    enum class ScenePart : std::size_t
    {
        Positions,
        Normals,
        TexCoords,
        Indices,

        /// One row per mesh slot: where its geometry sits and what it wears.
        Meshes,

        /// One row per placement slot: where it stands, what it is and what it wears.
        Instances,

        /// Where each placement stood last frame, which is what a motion vector is the difference
        /// of.
        Previous,

        Materials,
        Layers,
        Masks,
        Textures,
        Lights,
        Sprites,
        Emitters,

        /// What disturbed the water this frame, which the ripple field is stepped by.
        Ripples,

        /// What poses a mesh that deforms: the deformers, a rig's runs and influences and a
        /// morph's offsets.
        Deformers,

        /// The poses themselves, in the words both kinds are laid in.
        Poses,

        Count,
    };

    /// What a hashes file's header spells for `part`.
    std::string_view nameOf(ScenePart part);

    /// A digest of every part, indexed by `ScenePart`.
    using ScenePartDigests = std::array<std::array<std::uint64_t, 2>, static_cast<std::size_t>(ScenePart::Count)>;

    /// Digests each part of a scene on its own: the scene as the renderer will read it, every
    /// table in order and the shared geometry buffers included. Answers "is this the same
    /// buffer", which is what a repeat wants because the acceleration structures are built over
    /// it, where `digestScene` answers "is this the same cell". Fields and not records wherever a
    /// record has padding, because the bytes between are whatever the allocator left.
    ///
    /// **Kept across frames, so a frame pays for what moved.** The four vertex tables are tens
    /// of megabytes and change only where a mesh appears: a release leaves a hole and the bytes
    /// in it, and the next mesh to land there is an appearance. They are hashed again where
    /// `MeshTable::getRevision` or their lengths say so and read from the cache otherwise. A
    /// column of rows is laid end to end and hashed once, not once per field: twenty-five
    /// thousand placements six fields each was a hundred and fifty thousand hashes a frame.
    class SceneDigester
    {
    public:
        /// Every part's digest of `scene` as it stands.
        const ScenePartDigests& digest(const SceneDesc& scene);

        /// How many times the vertex tables were hashed, which is once per change to them. Read
        /// by the tests and by nothing else.
        std::uint32_t getVertexHashes() const { return mVertexHashes; }

    private:
        /// What the four vertex columns were last hashed from. A mesh appears through the
        /// revision, and the lengths are what a table grown past its blocks says; nothing else
        /// writes them.
        struct Vertices
        {
            std::uint64_t mRevision = 0;
            std::array<std::size_t, 5> mLengths{};

            bool operator==(const Vertices&) const = default;
        };

        void digestVertices(const SceneDesc& scene);

        void take(ScenePart part, const std::array<std::uint64_t, 2>& words)
        {
            mParts[static_cast<std::size_t>(part)] = words;
        }

        /// Empty until the first digest.
        std::optional<Vertices> mVertices;
        std::uint32_t mVertexHashes = 0;
        ScenePartDigests mParts{};

        /// A column's rows laid end to end, hashed once. Cleared and refilled, never freed.
        std::vector<std::byte> mScratch;
    };

    /// The one-off form `scene` and `shot` report with: a digester made and used once.
    ScenePartDigests digestParts(const SceneDesc& scene);

    /// One number for what a scene is made of, the same for two stagings of one cell. Per
    /// placement and summed, because `SceneUtil::Optimizer` merges sibling shapes in heap order and
    /// the vertex runs and slot numbers follow: each placement is digested from where it stands,
    /// what it wears and the multiset of its triangles, and no order can tell the sum. Blind to
    /// the layout, which is what `digestLayout` stands beside it for. Textures by their paths.
    std::array<std::uint64_t, 2> digestScene(const SceneDesc& scene);

    /// One number for the whole layout, for a caller with one line to print; a report with room
    /// for the columns names them instead.
    std::array<std::uint64_t, 2> digestLayout(const ScenePartDigests& parts);

    /// MurmurHash3 over whatever is fed to it, in the order it is fed. Chained through the seed, so
    /// a digest of many spans needs no copy of them laid end to end.
    class Digest
    {
    public:
        void add(std::span<const std::byte> bytes);

        template <class T>
        void add(std::span<const T> values)
        {
            add(std::as_bytes(values));
        }

        template <class T>
        void add(const T& value)
        {
            add(std::span<const T>(&value, 1));
        }

        const std::array<std::uint64_t, 2>& getWords() const { return mWords; }

    private:
        std::array<std::uint64_t, 2> mWords{};
    };

    /// Thirty-two hex digits, which is how a hashes file spells one and how `scene` reports one.
    std::string spellHash(const std::array<std::uint64_t, 2>& words);

}
