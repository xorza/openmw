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

    /// One thing a scene holds, and one column of a hashes file.
    ///
    /// **A digest each rather than one for the lot, because one number cannot say what moved.** A
    /// run whose layout differs is a run somebody now has to bisect, and every bisection of it asks
    /// the same question: which table. Answering it by rebuilding with the others switched off cost
    /// a run apiece and gave one reading each, which is a coin flip on a defect that appears in half
    /// the pairs. A column each answers it from the two files a single pair already wrote — and it
    /// found two: `island-crossing` differed in the materials and the textures on every pair, and in
    /// the geometry on some of them, which are two defects and not one.
    ///
    /// **In the order the tables are laid out**, so that reading the columns left to right is
    /// reading the scene the way the renderer does.
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
    /// in the order it is laid out and the shared geometry buffers included.
    ///
    /// **The question `digestScene` refuses, and a run has to ask both.** That one answers "is this
    /// the same cell", which is what a reference wants and what no permutation may change. These
    /// answer "is this the same buffer", which is what a repeat wants, because the buffer is what
    /// the acceleration structures are built over — and the layout differs on every frame of a
    /// route where `digestScene` matches on most of them.
    ///
    /// **Fields and not records, wherever a record has padding** — every table here has it but the
    /// geometry, and a `static_assert` holds the ones read whole to that. The bytes between fields
    /// are whatever the allocator left, and a hash that read them would call two identical scenes
    /// different.
    ///
    /// **A frame pays a hash of everything it is handed**, which is tens of megabytes of geometry
    /// and half again on the median frame. `bench --hashes` already
    /// reads every frame back and already says its times are not comparable with a measured run's,
    /// so it is the one caller that can afford this. **Whole and not incremental**, because the
    /// tables say which meshes deformed and not which arrived, and a second idea of when a slot
    /// changed is the copy of a fact this tree does not keep. A per-slot digest kept by `MeshTable`
    /// is what makes it incremental, and it is a change to the table rather than to this.
    ScenePartDigests digestParts(const SceneDesc& scene);

    /// One number for what a scene is made of, the same for two stagings of one cell.
    ///
    /// **Per placement and summed, because the host hands sibling shapes over in heap order.**
    /// `SceneUtil::Optimizer`'s `MergeGroupsVisitor` gathers the sibling groups that share a state
    /// set into a set of pointers, keeps the lowest-addressed as the survivor and appends the
    /// others' children behind it in address order — so two shapes under one placement change
    /// places from one process to the next, and the vertex runs and slot numbers follow them. Its
    /// `MergeGeometryVisitor` does the same inside a shape: the parts it folds into one geometry
    /// are concatenated in an order that is not the file's, so the same hair comes out with its
    /// vertices in two orders depending on nothing but the length of the working directory's name.
    /// So each placement is digested from what the picture is made of — where it stands, what it
    /// wears, and its shape as the multiset of its triangles — and the placements are summed, which
    /// no order can tell. Lights and emitters the same way.
    ///
    /// **What it is blind to, and why `digestLayout` stands beside it.** This names one cell one
    /// way whichever process staged it, and says nothing about the layout — which does matter to a
    /// ray tracer: a structure is built over the index buffer as written. Two runs whose triangles
    /// are the same in a different order draw a foliage edge two ways, and this reports them
    /// identical while they do.
    ///
    /// **Fields and not records, wherever a record has padding.** The bytes between fields are
    /// whatever the allocator left, and a hash that read them would call two identical scenes
    /// different. Textures by their paths and not their slots, for the same reason as the shapes.
    ///
    /// **Words and not a spelling, as below**: a caller keeps one beside a picture's hash as often
    /// as it prints one, and `spellHash` is the one way either becomes a file.
    std::array<std::uint64_t, 2> digestScene(const SceneDesc& scene);

    /// One number for the whole layout: every part of `parts`, folded in the order they are laid
    /// out.
    ///
    /// **A summary of what `digestParts` already answered**, for a caller with one line to print.
    /// A report with room for the columns names them instead, because which table moved is the
    /// question this number cannot answer.
    std::array<std::uint64_t, 2> digestLayout(const ScenePartDigests& parts);

    /// MurmurHash3 over whatever is fed to it, in the order it is fed.
    ///
    /// **Chained through the seed, which is why this fork's seed is two words**: each span is hashed
    /// with the result so far as the seed, so a digest of many spans needs no copy of them laid end
    /// to end. `FrameHashes` feeds it one frame's pixels; `scene` feeds it a whole description, a
    /// field at a time.
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
    /// run's hashes say about this one.
    ///
    /// **`verify` for a run rather than a view.** `verify` renders sixteen standing views and
    /// compares every pixel against a stored reference, which settles anything a single frame can
    /// show. It shows nothing that needs a second frame — a table copy a placement missed, a history
    /// reprojected onto the wrong surface, a row two frames stale — and those are where the defects
    /// are. Judging a moving run by a summary of a frame instead is judging it by a number with no
    /// expected value, which cannot tell a stale table from a camera that moved.
    ///
    /// **The scene beside the picture, because a picture that moved says nothing about why.** A run
    /// that differs has either drawn one scene two ways or been handed two scenes, and those are
    /// repaired in different places. `island-crossing` differed on 37 frames of 360 while what it
    /// was handed differed on all 360, which named the world rather than the renderer and was what
    /// the report could not say before.
    ///
    /// **A column per part of the scene and not one number for the lot, because "the layout moved"
    /// names no table.** `Rtx::ScenePart` says why. What a file holds is a header row and then a row
    /// a frame, so two runs are compared column by column and a report says which of them moved.
    ///
    /// **The parts and nothing dearer than they are.** `Rtx::digestScene` answers the other
    /// question — whether the two runs held one world or two — and it costs a walk of every triangle
    /// of every placement, which is a hashed run taking an order of magnitude longer. `scene` and
    /// `verify` ask it once at a place, which is where it is affordable.
    ///
    /// **A hash and not a picture**, because six hundred frames at 1920x1080 is a few hundred
    /// megabytes and the sixteen stills are kilobytes. What this answers is "did the run draw the
    /// same frames", and it names the ones that changed; what it cannot answer is by how much, and
    /// a frame it names is then rendered on its own for a look.
    class FrameHashes
    {
    public:
        /// Reads what a previous run wrote. Throws `Rtx::Error` where the file will not parse, so a
        /// reference that was truncated is a failure and not a run that silently matches nothing.
        static FrameHashes read(const std::filesystem::path& file);

        /// @param pixels **as the tool would write them to a PNG**, so a hash names the picture a
        ///        person would look at rather than an internal channel that may not survive a
        ///        rebuild.
        /// @param parts what `Rtx::digestParts` made of the description that drew them, one column
        ///        each.
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

            /// Frames where any part of the scene differs, in order.
            ///
            /// **Reported and not judged.** Two builds are expected to lay a scene out differently
            /// and to draw the same picture from it, so a difference here is what a reader needs to
            /// know and never what fails a run — `same` says so by leaving it out.
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
