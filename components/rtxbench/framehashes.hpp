#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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

        /// What poses a mesh that deforms: the rigs, their runs and their influences.
        Rigs,

        /// The morph targets and the offsets they move by.
        Morphs,

        /// The pose itself: a bone and a weight per influence.
        Bones,

        Count,
    };

    /// What a hashes file's header spells for `part`.
    std::string_view nameOf(ScenePart part);

    /// A digest of every part, indexed by `ScenePart`.
    using ScenePartDigests = std::array<std::array<std::uint64_t, 2>, static_cast<std::size_t>(ScenePart::Count)>;

    /// Digests each part of `scene` on its own: the scene as the renderer will read it, every table
    /// in order and the shared geometry buffers included. Answers "is this the same buffer", which
    /// is what a repeat wants because the acceleration structures are built over it, where
    /// `digestScene` answers "is this the same cell". Fields and not records wherever a record has
    /// padding, because the bytes between are whatever the allocator left. Whole and not
    /// incremental, and tens of megabytes a frame, which only `bench --hashes` can afford.
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

    /// Two hashes a frame of a run — what it drew and what it was handed — and what a previous
    /// run's hashes say about this one: `verify` for a run rather than a view, because a stale
    /// table or a history reprojected onto the wrong surface needs a second frame to show. The
    /// scene beside the picture, because a run that differs has either drawn one scene two ways or
    /// been handed two scenes, and those are repaired in different places; a column per part,
    /// because "the layout moved" names no table. A hash and not a picture, because six hundred
    /// frames is a few hundred megabytes; a frame it names is then rendered on its own for a look.
    class FrameHashes
    {
    public:
        /// Reads what a previous run wrote. Throws `Rtx::Error` where the file will not parse, so a
        /// truncated reference is a failure and not a run that silently matches nothing.
        static FrameHashes read(const std::filesystem::path& file);

        /// `pixels` as the tool would write them to a PNG, so a hash names the picture a person
        /// would look at; `parts` is what `Rtx::digestParts` made of the description that drew them.
        void add(std::string_view view, std::uint32_t frame, std::span<const std::uint8_t> pixels,
            const ScenePartDigests& parts);

        void write(const std::filesystem::path& file) const;

        std::size_t frameCount() const { return mFrames.size(); }

        /// What one view came to against `reference`.
        struct ViewDifference
        {
            std::string mView;
            std::uint32_t mFrames = 0;

            /// Frames whose picture differs, in order.
            std::vector<std::uint32_t> mDiffering;

            /// Frames where any part of the scene differs, in order. Reported and not judged: two
            /// builds may lay a scene out differently and draw the same picture, so `same` leaves
            /// it out.
            std::vector<std::uint32_t> mSceneDiffering;

            /// How many frames each part differs on, indexed by `ScenePart`.
            std::array<std::uint32_t, static_cast<std::size_t>(ScenePart::Count)> mPartsDiffering{};

            /// Frames this run drew that the reference has no hash for, and the other way about.
            std::uint32_t mUnmatched = 0;

            bool same() const { return mDiffering.empty() && mUnmatched == 0; }
        };

        /// One entry per view this run drew, in the order it drew them.
        std::vector<ViewDifference> against(const FrameHashes& reference) const;

    private:
        struct Frame
        {
            std::string mView;
            std::uint32_t mFrame = 0;
            std::array<std::uint64_t, 2> mHash{};
            ScenePartDigests mParts{};
        };

        std::vector<Frame> mFrames;
    };

    /// The differing frames of one view, as a line for the report — or empty where it matched.
    std::string describeDifference(const FrameHashes::ViewDifference& difference);
}
