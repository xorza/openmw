#pragma once

#include <array>
#include <cstdint>

namespace Rtx
{
    class SceneDesc;

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

    /// One number for the scene as the renderer will read it: every table in the order it is laid
    /// out, the shared geometry buffers included.
    ///
    /// **The question `digestScene` refuses, and a run has to ask both.** That one answers "is this
    /// the same cell", which is what a reference wants and what no permutation may change. This one
    /// answers "is this the same buffer", which is what a repeat wants, because the buffer is what
    /// the acceleration structures are built over. Measured on `island-crossing`, this differed on
    /// 360 frames of 360 while `digestScene` matched on 299 of them.
    ///
    /// **Fields and not records, for the reason above** — every table here has padding but the
    /// geometry, and a `static_assert` holds the ones read whole to that.
    ///
    /// **A frame pays a hash of everything it is handed**, which at that place is eighty megabytes
    /// of geometry and moved the median frame from 36.4 ms to 57.6 ms. `bench --hashes` already
    /// reads every frame back and already says its times are not comparable with a measured run's,
    /// so it is the one caller that can afford this. **Whole and not incremental**, because the
    /// tables say which meshes deformed and not which arrived, and a second idea of when a slot
    /// changed is the copy of a fact this tree does not keep. A per-slot digest kept by `MeshTable`
    /// is what makes it incremental, and it is a change to the table rather than to this.
    std::array<std::uint64_t, 2> digestLayout(const SceneDesc& scene);
}
