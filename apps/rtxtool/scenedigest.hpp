#pragma once

#include <string>

namespace Rtx
{
    class SceneDesc;
}

namespace RtxTool
{
    /// One number for what a scene hands the renderer, the same for two stagings of one cell.
    ///
    /// **Per placement and summed, because the host hands sibling shapes over in heap order.**
    /// `SceneUtil::Optimizer`'s `MergeGroupsVisitor` gathers the sibling groups that share a state
    /// set into a set of pointers, keeps the lowest-addressed as the survivor and appends the
    /// others' children behind it in address order — so two shapes under one placement change
    /// places from one process to the next, and the vertex runs and slot numbers follow them. Its
    /// `MergeGeometryVisitor` does the same inside a shape: the parts it folds into one geometry
    /// are concatenated in an order that is not the file's, so the same hair came out with its
    /// vertices in two orders depending on nothing but the length of the working directory's name.
    /// The picture does not change either way — the shapes share the placement, and a triangle is
    /// a triangle wherever its corners are stored. A digest over the buffers as laid out named the
    /// same scene two ways, and cost an investigation. So each placement is digested from what the
    /// picture is made of — where it stands, what it wears, and its shape as the multiset of its
    /// triangles — and the placements are summed, which no order can tell. Lights and emitters the
    /// same way.
    ///
    /// **Fields and not records, wherever a record has padding.** The bytes between fields are
    /// whatever the allocator left, and a hash that read them would call two identical scenes
    /// different. Textures by their paths and not their slots, for the same reason as the shapes.
    std::string digestScene(const Rtx::SceneDesc& scene);
}
