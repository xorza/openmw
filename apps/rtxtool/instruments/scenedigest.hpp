#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <components/rtx/namedenum.hpp>
#include <components/rtx/shaders/visibility.h>

namespace Rtx
{
    class SceneDesc;
}

namespace RtxTool
{
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

        /// What the frame was traced with beside the scene: the camera, the sky, the air, the sea
        /// and the sample, as `Shaders::VisibilityConstants` — the one input to a trace the scene
        /// does not hold. A picture that moved while every column before this stood is a frame
        /// column that moved, a history's, or the code the launches compiled to —
        /// `PipelineCacheSpec::mDirectory` says how one build compiles to two. Nought where no
        /// frame was traced, which is what `scene` reports.
        Frame,

        Count,
    };

    /// What a hashes file's header spells for each part, in the order the tables are laid out,
    /// which is the order of the columns: the header is written from this table and a column is
    /// indexed by the enumerator, so the two must agree, and the assertion below says they do.
    inline constexpr Rtx::NamedEnum sSceneParts{ std::array{
        std::pair{ ScenePart::Positions, std::string_view("positions") },
        std::pair{ ScenePart::Normals, std::string_view("normals") },
        std::pair{ ScenePart::TexCoords, std::string_view("texcoords") },
        std::pair{ ScenePart::Indices, std::string_view("indices") },
        std::pair{ ScenePart::Meshes, std::string_view("meshes") },
        std::pair{ ScenePart::Instances, std::string_view("instances") },
        std::pair{ ScenePart::Previous, std::string_view("previous") },
        std::pair{ ScenePart::Materials, std::string_view("materials") },
        std::pair{ ScenePart::Layers, std::string_view("layers") },
        std::pair{ ScenePart::Masks, std::string_view("masks") },
        std::pair{ ScenePart::Textures, std::string_view("textures") },
        std::pair{ ScenePart::Lights, std::string_view("lights") },
        std::pair{ ScenePart::Sprites, std::string_view("sprites") },
        std::pair{ ScenePart::Emitters, std::string_view("emitters") },
        std::pair{ ScenePart::Ripples, std::string_view("ripples") },
        std::pair{ ScenePart::Deformers, std::string_view("deformers") },
        std::pair{ ScenePart::Poses, std::string_view("poses") },
        std::pair{ ScenePart::Frame, std::string_view("frame") },
    } };

    consteval bool scenePartsInOrder()
    {
        const auto values = sSceneParts.values();
        for (std::size_t at = 0; at < values.size(); ++at)
            if (static_cast<std::size_t>(values[at]) != at)
                return false;

        return values.size() == static_cast<std::size_t>(ScenePart::Count);
    }

    static_assert(scenePartsInOrder(), "the parts table is the columns, so it lists every part in enum order");

    /// What a hashes file's header spells for `part`.
    constexpr std::string_view nameOf(const ScenePart part)
    {
        return sSceneParts.name(part);
    }

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
        /// Every part's digest of `scene` as it stands, and of `frame`, the constants it is traced
        /// with, or null where nothing is traced.
        const ScenePartDigests& digest(
            const Rtx::SceneDesc& scene, const Rtx::Shaders::VisibilityConstants* frame = nullptr);

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

        void digestVertices(const Rtx::SceneDesc& scene);

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
    ScenePartDigests digestParts(const Rtx::SceneDesc& scene);

    /// One number for what a scene is made of, the same for two stagings of one cell. Per
    /// placement and summed, because `SceneUtil::Optimizer` merges sibling shapes in heap order and
    /// the vertex runs and slot numbers follow: each placement is digested from where it stands,
    /// what it wears and the multiset of its triangles, and no order can tell the sum. Blind to
    /// the layout, which is what `digestLayout` stands beside it for. Textures by their paths.
    std::array<std::uint64_t, 2> digestScene(const Rtx::SceneDesc& scene);

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
