#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

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
    /// the acceleration structures are built over. Measured on `island-crossing`, the layout
    /// differed on 360 frames of 360 while `digestScene` matched on 299 of them.
    ///
    /// **Fields and not records, wherever a record has padding** — every table here has it but the
    /// geometry, and a `static_assert` holds the ones read whole to that. The bytes between fields
    /// are whatever the allocator left, and a hash that read them would call two identical scenes
    /// different.
    ///
    /// **A frame pays a hash of everything it is handed**, which at that place is eighty megabytes
    /// of geometry and moved the median frame from 36.4 ms to 57.6 ms. `bench --hashes` already
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
    /// are concatenated in an order that is not the file's, so the same hair came out with its
    /// vertices in two orders depending on nothing but the length of the working directory's name.
    /// So each placement is digested from what the picture is made of — where it stands, what it
    /// wears, and its shape as the multiset of its triangles — and the placements are summed, which
    /// no order can tell. Lights and emitters the same way.
    ///
    /// **What that blindness cost, and why `digestLayout` stands beside it.** This was written to
    /// name one cell one way whichever process staged it, and it does. It was also read as saying
    /// the layout does not matter, which is false for a ray tracer: a structure is built over the
    /// index buffer as written. Two runs whose triangles are the same in a different order draw a
    /// foliage edge two ways, and this reported them identical while they did.
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
}
