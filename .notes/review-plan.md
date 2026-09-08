# Implementation plan for `.notes/review-modules.md`

Delete a stage when it is landed and verified. This file lists open work only.

Each stage is one commit. A stage names the review items it closes, the files it touches, the shape
of the change, the tests it owes and the command that verifies it. Stages 1 to 4 are independent of
each other. Stage 5 onwards depend on the new types stages 3 and 4 introduce.

**One rule across every stage.** No stage may change what a frame draws. Two stages change what a
*digest* spells, and each says so; nothing in this fork stores a digest, so that is safe — the
hashes are compared between two runs of one binary and never against a recorded value.

---

## Stage 1 — Close the free-list defect

Closes: the three items under "A free list is filled as a stack and read as a heap".

**The defect.** `DeformerTable::release` puts a rig slot back with `push_back`. `addRig` takes one
through `takeSlot`, which calls `std::pop_heap`. `std::pop_heap` requires a heap. A vector filled by
`push_back` is not one, so the call is undefined and the slot it answers with is not the lowest
free one. `slotrows.hpp:23` states the rule this breaks.

**The change.** Two lines.

```cpp
// components/rtx/deformertable.cpp:137
freeSlot(mFreeRigs, range.mDeformer);

// components/rtx/deformertable.cpp:151
freeSlot(mFreeMorphs, range.mDeformer);
```

`slotrows.hpp` is already included by `deformertable.cpp`.

**Why no test catches it today.** `apps/components_tests/rtx/scenedesc.cpp:453` frees one rig and
asserts the next arrival takes that slot. With one slot on the list, a heap and a stack answer the
same. The defect needs two.

**The test.** Extend `RtxSkinnedMeshTest` on its own fixture rather than adding a file. Build three
rigs, free the highest and then the lowest, and assert the next arrival takes the lowest. Show the
arithmetic in the message: a stack answers with the last one freed, and a heap answers with the
lowest one free.

**Verify.**

```
cmake --build build-debug --target components-tests
./build-debug/components-tests --gtest_filter='RtxSkinnedMeshTest.*:RtxSceneDescTest.*'
```

**And then the gate**, because this changes which slot a scene hands out:

```
./apps/rtxtool/repeatable.sh --pairs=10
```

Record the reading in `.notes/repeatable.txt` with the commit it was taken at.

---

## Stage 2 — Make the repeatability gate cover the whole struct

Closes: the three items under "The repeatability gate lists struct fields by hand".

**The problem.** `components/rtxbench/scenedigest.cpp` names the fields of `MeshRange`,
`MeshInstance`, `Material` and `SpriteEmitter` by hand, and names `Material` twice with two
different subsets. A field added to any of them compiles, runs and drops out of the gate in
silence. The gate is what `repeatable.sh` reports and what every determinism argument rests on.

**The technique is already in the tree.** `components/rtx/extractionstats.cpp:16` destructures the
whole struct with a structured binding, so a field added to `ExtractionStats` does not compile until
it is named. Every type above is an aggregate with public members, no bases and no virtuals, so the
same binding works on each.

**The design.** One field list per type, in `scenedigest.cpp`, in an anonymous namespace.

```cpp
/// Every field of a mesh row, as one list.
///
/// **A field added to `MeshRange` and not named here does not compile**, which is what keeps the
/// gate covering the whole row. `ExtractionStats::countersOf` makes the same argument for the
/// same reason.
auto fieldsOf(const MeshRange& mesh)
{
    const auto& [vertices, indices, shape, deform, deformer, material, bind, pose, posed, bounds] = mesh;
    return std::tie(vertices, indices, shape, deform, deformer, material, bind, pose, posed, bounds);
}

/// Hashes every field of `fields`, in the order the list gives them.
void addFields(Digest& digest, const auto& fields)
{
    std::apply([&](const auto&... field) { (digest.add(field), ...); }, fields);
}
```

`MeshInstance` (5 members), `SpriteEmitter` (7) and `Material` (14) get the same pair. `digestParts`
then reads:

```cpp
for (const MeshRange& mesh : scene.getMeshes())
    addFields(one, fieldsOf(mesh));
take(ScenePart::Meshes);
```

**`Material` needs two spellings and one list.** `digestParts` hashes a texture by its slot, because
which slot a texture landed in is what a material's index means. `addMaterial` hashes it by its
path, because `digestScene` must not see the order the slots were handed out in. So the shared
thing is the field list and not the digest of it:

```cpp
/// Hands every field of `material` to `visit`. The texture slots go to `visit.texture` rather
/// than to `visit.value`, because the two digests spell a texture differently — one by the slot
/// it landed in, and one by the file it names.
///
/// **A field added to `Material` and not named here does not compile.**
template <class Visit>
void forEachMaterialField(const Material& material, Visit&& visit)
{
    const auto& [kind, diffuse, normal, emissive, diffuseColour, emissiveColour, alphaRef, alphaMode,
        twoSided, textureTransform, layers, flatten, animated, neverSolid] = material;

    visit.texture(diffuse);
    visit.texture(normal);
    visit.texture(emissive);
    visit.value(kind);
    ...
}
```

`addMaterial`'s visitor sends `.texture` to the existing `addTexture`. `digestParts`'s visitor sends
it to `digest.add`. Both send `.value` to `digest.add`.

**The padding guard.** `Digest::add` hashes an object representation, so a padding byte would be
hashed as well, and a padding byte holds nothing anyone wrote. Seven scene types reach the digest
whole: `Light`, `Rig`, `Morph`, `Sprite`, `MaterialLayer`, `Shaders::GpuBone` and
`Shaders::GpuInfluence`. None has padding today, and every one of them is padding-free only because
of its current field order. Put one assertion per type beside the calls:

```cpp
// **Hashed as bytes, padding included, and none of these has any.** One `bool` added to any of
// them puts a padding byte in the digest, and a padding byte holds whatever the allocator left —
// which would have the gate report a difference the renderer did not make. A size that moves is
// where to decide whether the type is still packed.
static_assert(sizeof(Light) == 36, "a light with padding in it");
static_assert(sizeof(Rig) == 16, "a rig with padding in it");
...
```

**What this changes.** The digest values move, because `mBounds` and `mLayers` are now hashed whole
rather than a half at a time. Nothing stores a digest:
`apps/components_tests/rtxbench/scenedigest.cpp` compares digests against each other, and
`repeatable.sh` compares two runs of one binary. No reference file in the tree holds one.

**Verify.**

```
cmake --build build-debug --target components-tests
./build-debug/components-tests --gtest_filter='RtxSceneDigestTest.*:RtxFrameHashesTest.*'
./apps/rtxtool/repeatable.sh --pairs=10
```

---

## Stage 3 — `SlotRows<Row>`: one table of slots, four users

Closes: the three items under "`MeshTable` and `MaterialTable` are the same table twice", and the
hand-rolled slot taking in `TextureTable` and `PlacementTable`.

**The problem.** Four tables each hold `std::vector<Row> mRows` beside `std::vector<Index> mFree`,
and each spells the same three things: take a free slot or grow, mark what a sweep keeps, and free
what it did not. `MeshTable::mark` and `MaterialTable::mark` have identical bodies, as do their
`getLiveCount`. Two of the four take a slot through `takeSlot` and two spell it out, because they
carry parallel arrays the shared helper cannot reach.

**The design.** A new `components/rtx/slotrows.hpp` type beside the free functions already there.
The growth hook is what lets the parallel-array tables use it.

```cpp
/// A table of fixed-size rows: the rows, the slots nothing stands in, and the sweep.
///
/// **One type, because four tables were the same three rules written four times.** A slot is never
/// moved and never closed up — a mesh index names a bottom-level structure and a texture index is
/// what a material points at — so a dropped row leaves a hole and the next arrival takes it. Which
/// hole it takes is the lowest, for the reason `takeFreeSlot` gives.
template <class Row>
class SlotRows
{
public:
    std::size_t size() const;

    /// How many slots hold a row, which is what a sweep compares its survivors against.
    std::size_t getLiveCount() const { return mRows.size() - mFree.size(); }

    std::span<const Row> getRows() const { return mRows; }
    Row& at(Index slot);
    const Row& at(Index slot) const;

    /// Puts `row` in a free slot, or in a new one.
    ///
    /// @param grew called with the table's new length wherever the table grew, so a caller
    ///        holding arrays parallel to this one follows in the same call. `TextureTable` keeps
    ///        two names beside its rows and `PlacementTable` keeps a previous transform.
    template <class Grew>
    Index take(const Row& row, Grew grew);

    Index take(const Row& row) { return take(row, [](std::size_t) {}); }

    /// Empties `slot` and puts it back. The row is left as `Row{}`.
    void free(Index slot);

    /// Notes every slot a sweep must not free, and says how many distinct ones `keep` named.
    ///
    /// **Apart from `sweep`, because a scene marks two tables and frees neither where both came
    /// back whole.**
    std::size_t mark(std::span<const Index> keep);

    /// Frees every slot the last `mark` did not name, and says how many that was.
    ///
    /// @param release `void(Index, Row&)`, called before each row is freed. What a row named
    ///        goes back there — a mesh's runs, a material's textures — because only the table
    ///        that owns the row knows what it owns.
    template <class Release>
    std::size_t sweep(Release release);

private:
    std::vector<Row> mRows;

    /// A min-heap. `Rtx::takeFreeSlot` says why the lowest.
    std::vector<Index> mFree;

    /// Which slots the last `mark` named, one flag per row. Grown by `mark` and by nothing else,
    /// so a table that never sweeps never allocates it.
    std::vector<std::uint8_t> mKept;
};
```

**What each table becomes.**

- `MeshTable`: `SlotRows<MeshRange> mRows`. Its `sweep` callback releases the vertex and index runs
  and the deformer, clears the row's counts, takes the slot out of `mDeformed` and notes it freed.
  `mark`, `getLiveCount`, `size` and `getRows` become one-line forwards.
- `MaterialTable`: `SlotRows<Material> mRows`. Its `sweep` callback drops the textures, releases the
  mask runs and the layer run.
- `TextureTable`: `SlotRows<Slot> mSlots`. `takeSlot` becomes a `take` whose `grew` resizes
  `mPaths`, `mBaked` and `mChanges`. `drop` calls `free`.
- `PlacementTable`: `SlotRows<MeshInstance> mInstances`. `add` passes a `grew` that resizes
  `mPrevious`. `drop` calls `free`. It never marks or sweeps, so `mKept` stays empty.

**The one behaviour this fixes on the way.** `TextureTable` and `PlacementTable` already take the
lowest free slot, so nothing moves for them. `MeshTable` and `MaterialTable` are unchanged too. The
change is structural.

**The test.** `apps/components_tests/rtx/scenedesc.cpp` already covers slot reuse for meshes,
materials, textures and placements through `SceneDesc`. Add a table-driven case to the nearest
existing fixture that frees two slots out of order and asserts the lowest comes back first, for each
of the four tables. That is the property `SlotRows` now promises in one place.

**Verify.**

```
cmake --build build-debug --target components-tests
./build-debug/components-tests --gtest_filter='RtxSceneDescTest.*:RtxSkinnedMeshTest.*:RtxSceneTableTest.*'
./apps/rtxtool/repeatable.sh --pairs=10
```

---

## Stage 4 — `RunBuffer<T>`: a run allocator and the buffer its runs name

Closes: the four items under "Allocate a run, grow the buffer, copy into it".

**The problem.** Nine sites repeat the same three steps: take a run from a `RunAllocator`, grow the
parallel buffer to `getEnd()` where it is short, then copy at the run's offset. A site that forgets
the growth writes past the end of a vector, and nothing says the two belong together.

**The design.** A new `components/rtx/runbuffer.hpp`.

```cpp
/// A `RunAllocator` and the buffer its runs name.
///
/// **One type, because the two were held in step by hand at nine places.** A run is taken from the
/// allocator and the buffer has to reach the allocator's end before anything is written into it;
/// the growth is not the caller's business and a caller that forgot it wrote past the end.
///
/// **Grown and never shrunk**, exactly as every site did: a run given back at the very end goes to
/// the allocator, and the next arrival lands in it rather than in a buffer resized twice.
template <class T>
class RunBuffer
{
public:
    /// @param block a boundary no run may straddle, or zero. `RunAllocator` says what it is for.
    explicit RunBuffer(std::uint32_t block = 0);

    /// Room for `count` elements. **The contents are whatever the last tenant left**, so a caller
    /// writes the whole run before anything reads it.
    Run allocate(std::uint32_t count);

    /// The same room, holding `values`.
    Run allocate(std::span<const T> values);

    /// The same room, value-initialised. `DeformerTable::stand` hands out a pose nothing can equal.
    Run allocateZeroed(std::uint32_t count);

    void release(Run run);

    std::span<const T> getAll() const;
    std::span<T> getAll();

    /// One run's elements, for a caller writing into what it took.
    std::span<T> in(Run run);

    std::uint32_t getEnd() const;
    std::uint32_t getUsed() const;
    std::size_t getHoleCount() const;

private:
    RunAllocator mRuns;
    std::vector<T> mValues;
};
```

**What each site becomes.**

| site | today | after |
| --- | --- | --- |
| `materialtable.cpp:65` `addMask` | allocate, grow, copy | `mMasks.allocate(weights)` |
| `materialtable.cpp:79` `addLayers` | allocate, grow, copy | `mLayers.allocate(layers)` |
| `deformertable.cpp:30` `addRig` runs | allocate, grow, copy | `mRuns.allocate(runs)` |
| `deformertable.cpp:32` `addRig` influences | allocate, grow, copy | `mInfluences.allocate(influences)` |
| `deformertable.cpp:54` `addMorph` | allocate, grow, copy | `mMorphOffsets.allocate(offsets)` |
| `deformertable.cpp:173` `stand` bones | allocate, grow, fill | `mBones.allocateZeroed(rig.mBoneCount)` |
| `deformertable.cpp:184` `stand` weights | allocate, grow, fill | `mWeights.allocateZeroed(morph.mTargetCount)` |
| `meshtable.cpp:70` indices | allocate, grow, copy | `mIndices.allocate(indices)` |
| `meshtable.cpp:63` vertices | allocate, grow three | see below |

**The vertex trio stays parallel by a simpler rule.** `MeshTable` holds one allocator over three
arrays, because a shader indexes all three by one vertex id. `mPositions` becomes a
`RunBuffer<osg::Vec3f>`, and `mNormals` and `mTexCoords` are resized to `mPositions.getAll().size()`
rather than to an allocator's end. The rule a reader has to hold then is "the attribute buffers are
as long as the position buffer", which is what the comment there already claims and is one fact
instead of two.

`DeformerTable::mBindRuns` stays a bare `RunAllocator`. It hands out offsets into a table the
*backend* owns, and there is no host buffer for it to grow. That is the case that says why
`RunBuffer` must not replace `RunAllocator` outright.

**The test.** `apps/components_tests/rtx/runallocator.cpp` covers the allocator. Add a `RunBuffer`
case to the same file: allocate two runs, release the first, allocate one that fits the hole, and
assert the buffer did not grow and that the second run's elements are where they were. That is the
invariant the nine sites were each keeping by hand.

**Verify.**

```
cmake --build build-debug --target components-tests
./build-debug/components-tests --gtest_filter='RtxRunAllocatorTest.*:RtxRunBufferTest.*:RtxSceneDescTest.*'
./apps/rtxtool/repeatable.sh --pairs=10
```

---

## Stage 5 — `SlotSet` for the deformer arrivals

Closes: the three items under "Change lists that do not use `SlotSet`".

**The problem.** `mArrivedRigs` and `mArrivedMorphs` are plain vectors, and
`deformertable.cpp:138` and `:152` call `std::erase` on them once per released deformer. That is a
linear scan and an erase from the middle of a list, per rig, on the frame a cell leaves — which is
the frame with the least room to spare. `SlotSet` exists for exactly this, and it de-duplicates as
well.

**`SlotSet` and not `SlotChanges`.** A rig's storage is a run in a shared buffer. `SkinTables::
writeRigs` writes a run when a rig arrives and never has to be told one went, because the next rig
to land in that run is what writes it again. So there is no freed list to keep and nothing for a
`SlotChanges` to carry.

**The change.**

```cpp
// deformertable.hpp
SlotSet mArrivedRigs;
SlotSet mArrivedMorphs;

// addRig / addMorph, after the slot is taken
mArrivedRigs.addMakingRoom(index);

// release, where the last mesh leaves
mArrivedRigs.remove(range.mDeformer);

// a new method, called where a sweep settles
void settle() { mArrivedRigs.compact(); mArrivedMorphs.compact(); }
```

`SlotSet::getSlots` refuses to answer while a removal is outstanding, so the compaction has to
happen before anything reads the arrivals. `MeshTable::sweep` already compacts `mDeformed` at its
tail (`meshtable.cpp:195`); add `mDeformers.settle()` on the line beside it. That is the one place a
deformer can be released.

**The placement lists.** `PlacementTable::mMoved` and `mSettled` hold duplicates on purpose, and the
header states the cost as one row written twice. Leave them, and add the missing half of that
sentence: say why the crowded cell `SlotSet` was measured on is not this case — a placement row is a
memcpy of about a hundred bytes and a duplicate is bounded by how many facts about one slot can
change in one frame, which is three.

**The test.** Extend the existing skinned-mesh fixture: release two rigs in one sweep and assert
that the arrivals are empty and that `getArrivedRigs` answers rather than trips its assertion.

**Verify.**

```
cmake --build build-debug --target components-tests
./build-debug/components-tests --gtest_filter='RtxSkinnedMeshTest.*:RtxSceneDescTest.*'
```

---

## Stage 6 — One way to record a compute dispatch

Closes: the five items under "The Vulkan backend records a compute pass by hand, once per pass".

**The problem.** `groupsFor` is defined in eleven files. The descriptor binding list, the write
loop and the four-line bind-and-dispatch block are repeated in nine to twelve. Two files already
carry the general form of `groupsFor`, and `exposurepass.cpp:31` already carries a local
`bufferWrite` — the abstraction is being reached for, one file at a time.

**The design.** A new `components/rtxvulkan/dispatch.hpp` of free functions. Free functions and not
a class: the passes differ in what they hold, and only in how they say these four things.

```cpp
/// How many workgroups cover `extent` lanes.
constexpr std::uint32_t groupsFor(std::uint32_t extent, std::uint32_t workgroup);

/// One binding of set zero, visible to the compute stage.
constexpr VkDescriptorSetLayoutBinding computeBinding(std::uint32_t slot, VkDescriptorType type);

/// `N` bindings of one type, numbered from zero.
///
/// **A loop and not a list, because eleven of one kind is not a list anybody reads.**
/// `AccumulatePass` already builds its eleven this way; the others spell theirs out.
template <std::size_t N>
constexpr std::array<VkDescriptorSetLayoutBinding, N> computeBindings(VkDescriptorType type);

VkWriteDescriptorSet imageWrite(std::uint32_t binding, const VkDescriptorImageInfo& info,
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
VkWriteDescriptorSet bufferWrite(std::uint32_t binding, const VkDescriptorBufferInfo& info,
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

/// Fills `writes` from `images`, binding `i` from image `i`. A pass whose set is not all storage
/// images fills the rest itself, as `TonePass` does for its buffer and its sampler.
void writeStorageImages(std::span<VkWriteDescriptorSet> writes, std::span<const VkDescriptorImageInfo> images);

/// Binds, pushes and launches. The four lines every pass ends with.
template <class Constants>
void dispatch(VkCommandBuffer commands, const ComputePipeline& pipeline,
    std::span<const VkWriteDescriptorSet> writes, const Constants& constants,
    std::uint32_t groupsX, std::uint32_t groupsY = 1, std::uint32_t groupsZ = 1);
```

**What it serves.** `computeBindings<N>(STORAGE_IMAGE)` serves `AccumulatePass` (11), `AtrousPass`
(5) and `CompositePass` (5) outright. `computeBinding` serves the mixed sets — `TonePass`,
`BloomPass`, `ExposurePass`, `GuiPass`, `FogVolume` — one line each instead of five. `dispatch` and
`groupsFor` serve every one of the twelve.

**Do this file by file and in one commit per group**, not as one sweep: each pass's barriers stay
exactly where they are, and only the four statements around the launch move. A pass whose set is
not uniform keeps its own list and still loses `groupsFor` and the launch block.

**The test.** `apps/components_tests/rtx/` holds a test per pass —
`bloompass.cpp`, `guipass.cpp`, `skinpass.cpp`, `spritebinpass.cpp`, `wavepass.cpp`. Those cover the
passes. Add one case beside `computepipeline.cpp` that asserts `groupsFor` covers its extent exactly
at a multiple of the workgroup and one lane past it, which is the arithmetic all twelve copies were
each carrying.

**Verify.**

```
cmake --build build-debug --target components-tests --target openmw-rtxtool
./build-debug/components-tests --gtest_filter='Rtx*PassTest.*:RtxComputePipelineTest.*'
./build-debug/openmw-rtxtool shot --view=balmora
./apps/rtxtool/repeatable.sh --pairs=10
```

The picture must not move. `shot` prints the hit fraction and the frame it drew; compare it against
the same call on the commit before.

---

## Stage 7 — One camera, three viewpoints

Closes: the item under "Three camera builders, one set of defaults, three copies of the comment".

**The design.** Two helpers in the anonymous namespace of `camera.cpp`.

```cpp
/// What every camera carries beyond its own basis and its own image plane.
///
/// **One statement, because three builders were carrying the same three fields and two of them
/// were carrying the same comment word for word.** `VisibilityConstants` is a shader header and
/// cannot hold default member initialisers, so the defaults live here instead.
Shaders::VisibilityConstants baseCamera(const ViewBasis& basis, float near, float far);

/// The half-extents of the image plane at one unit, and the angle one pixel covers.
struct Spread { float mHalfWidth; float mHalfHeight; float mAngle; };
Spread spreadOf(float fovDegrees, std::uint32_t width, std::uint32_t height);
```

`makeCameraFromView` and `makeCameraAlong` then differ only in where the basis comes from.
`makeOrthographicCameraFromView` keeps its own image-plane arithmetic — a parallel ray has no
spread — and takes the tail from `baseCamera`.

`makeCameraAlong` builds its basis from the world's up rather than from a matrix, so give it a
`ViewBasis` of its own and let it reach `baseCamera` the same way. That also puts the three
`Error` messages about a collapsed basis in one place.

**The test.** `RtxCameraTest`, in `apps/components_tests/rtx/visibility/frame.cpp`, already covers
the three builders and already cross-checks `makeCamera` against `makeCameraFromView` on one
viewpoint. That is the guard this stage needs, so no new file is owed. Add one assertion to it: the
three builders agree on the tail — the water level, the sea heading and the fog lift — which is
what one shared helper now has to keep true.

**Verify.**

```
cmake --build build-debug --target components-tests
./build-debug/components-tests --gtest_filter='RtxCameraTest.*:RtxFrameWorldTest.*'
```

---

## Stage 8 — `MeshResolver::resolve` takes one path through the deformers

Closes: the two items under "`MeshResolver::resolve` asks the same question five times".

**The problem.** `meshresolver.cpp:180` is 174 lines and branches on `read.mDeform` at five points:
the reuse test, the deformer lookup, the pose on reuse, the deformer resolve and the pose on
arrival. The two pose blocks are the same six lines twice. `DeformerTable::stand` and `release` each
carry a Rig arm and a Morph arm that differ only in which table and which allocator they name.

**The design, in `MeshResolver`.** A small held object, so the entry the lookup found is the entry
the stamp uses — which is the property the current code spends a paragraph defending.

```cpp
/// What poses one drawable, as the mirror already holds it.
///
/// **The entry and not only the index**, because the stamp wants the one the lookup found: a
/// second `find` per posed part per frame is a pointer hash and a bucket walk for an answer
/// already in hand, and Vivec poses 332.
struct MeshResolver::Held
{
    /// `sNoIndex` where the mirror has not met this deformer, or where the drawable stands.
    Index mIndex = sNoIndex;

    Identity<const SceneUtil::RigGeometry::InfluenceData>::Entry mRig;
    Identity<const osg::Vec3Array>::Entry mMorph;
};

/// The deformer this drawable stands on, where the mirror holds it. **Stamps nothing**: the fit
/// test decides whether the slot survives, and a stamp before it would keep a slot that does not.
Held holdDeformer(const Read& read);

/// Stamps what `holdDeformer` found and poses `mesh` from it. Only for a slot known to fit.
void keepPosed(Index mesh, const Read& read, const Held& held, ExtractionStats& stats);

/// Poses `mesh` where the drawable deforms, and counts it. The arrival path, where `resolveRig`
/// and `resolveMorph` have already stamped through `reach`.
void posed(Index mesh, const Read& read, ExtractionStats& stats);
```

`resolve` then reads as three steps: reuse where the slot fits, abandon where it does not, add
otherwise. The `Deform` switch happens twice — once in `holdDeformer` and once in the pose — and
each is exhaustive in one place.

**`DeformerTable::stand` and `release`.** Both arms name a deformer table, a use count, a pose
allocator and a pose buffer. After stage 4 the pose allocators are `RunBuffer`s, so a small private
struct of references collapses the pair:

```cpp
/// The four things a deformer's arm names, so `stand` and `release` are written once.
struct DeformerTable::Standing
{
    Index& mUses;
    Index mPoseCount;
    RunBuffer<Shaders::GpuBone>* mBones;   // exactly one of the two
    RunBuffer<float>* mWeights;
};
```

That one is worth taking only if it reads better than the two arms do. Write it, read it, and keep
the arms if it does not. Say which was chosen in the commit message.

**The test.** `apps/components_tests/rtx/extractor/` holds the resolver tests. The behaviour must not
move at all, so no new test is owed — but add one case the current shape makes easy to get wrong: a
rigged drawable whose source geometry is replaced with a longer one under the same address, which is
the path the fit test exists for. Assert the mesh is mirrored afresh and the old slot is swept.

**Verify.**

```
cmake --build build-debug --target components-tests
./build-debug/components-tests --gtest_filter='RtxSceneExtractorTest.*:RtxSkinnedMeshTest.*'
./apps/rtxtool/repeatable.sh --pairs=10
```

---

## Stage 9 — One shape for the three material resolvers

Closes: the item under "Three resolve functions with one shape".

**The design.** Two private methods in `MaterialResolver`.

```cpp
/// The slot for `key` where the mirror holds one, stamped and counted as a reuse. `sNoIndex`
/// otherwise.
Index reuse(const osg::StateSet* key);

/// Adds `material` under `key`, counted as an arrival, and hands back its slot.
Index adopt(const osg::StateSet* key, const Material& material);
```

Each of `resolve`, `resolveTerrain` and `resolveWater` then opens with `reuse` and closes with
`adopt`, and holds nothing between them but what it actually does differently: `resolve` re-reads an
animated material, `resolveTerrain` builds a layer stack, `resolveWater` builds nothing.

**No test is owed.** The three functions keep their behaviour exactly. The existing extractor tests
cover the counts.

**Verify.**

```
cmake --build build-debug --target components-tests
./build-debug/components-tests --gtest_filter='RtxSceneExtractorTest.*'
```

---

## Stage 10 — Casts that agree with the file they are in

Closes: the three items under "Casts that the file next to them argues against".

Three small changes, one commit.

- **`components/rtx/meshresolver.cpp:308`.** Add `asVec2Array` beside `asVec3Array`, keyed on
  `osg::Array::Vec2ArrayType`, and read the texture coordinates through it. `asVec3Array` already
  carries the argument; the second array type was reading itself the slow way three lines below it.
- **`components/rtx/shading.cpp:27`.** `getAttribute(osg::StateAttribute::BLENDFUNC)` is keyed by
  type, so `static_cast` is exact. `addsLight` runs once per drawable's chain, which is the frame
  path.
- **`components/rtx/sceneextractor.cpp`.** Add one helper and use it at the five gated casts.

  ```cpp
  /// `object` as a `T`, but only where its library says it could be one.
  ///
  /// **The gate and the cast in one call**, because they were spelled apart at five places and a
  /// gate is only useful where nobody can forget it. `NodeLibrary` says why the compare is worth
  /// making: a failed `dynamic_cast` walks the class hierarchy to answer what one compare does.
  template <class T, class Object>
  T* castFrom(Library held, Library wanted, Object& object)
  {
      return held == wanted ? dynamic_cast<T*>(&object) : nullptr;
  }
  ```

  `stepParticles` gates its pair at the head of the function instead, and stays as it is.

**The test.** `apps/components_tests/rtx/nodelibrary.cpp` covers the gate. Add a case asserting that
a drawable carrying a two-component texture-coordinate array is read, and one carrying a
three-component one is not — which is the behaviour `asVec2Array` has to keep.

**Verify.**

```
cmake --build build-debug --target components-tests
./build-debug/components-tests --gtest_filter='RtxNodeLibraryTest.*:RtxSceneExtractorTest.*'
```

---

## Stage 11 — The small ones

Closes: the item under "An invariant kept by two loops agreeing", and the four under "Small
duplication and stale narration". One commit.

- **`components/rtx/texturebuilder.cpp:249`.** Every `TextureData` already built spans `mLevels`, and
  the reserve is computed by one loop while the fill runs through another. Hold the capacity across
  the fill and assert it:

  ```cpp
  const std::size_t reserved = mLevels.capacity();
  ...
  assert(mLevels.capacity() == reserved && "the level table grew while descriptions spanned it");
  ```

  That is exact: a growth is the failure, and a growth is what changes the capacity.
- **`megabytes`.** Move the one definition to `components/rtx/memoryreport.hpp` as an inline, and
  delete the copy in `components/rtxbench/benchrecord.cpp:14`. `rtxbench` already links `rtx`.
- **`apps/openmw/mwrender/rtx/rtxrenderer.hpp:350`.** `mSpentMs` sums `result->mWaitMs` and the log
  line says so. Correct the member's comment to say what the number is: what the CPU stood still for
  the device, over the frames a report came back for.
- **`components/myguirtx/rendermanager.hpp:122`.** Delete the doc comment. It describes a clock that
  left when `update` started taking its step from the caller.
- **`RtxRenderer::renderGui`.** It draws the interface and presents, and it is the ordinary present
  path for a frame that has a world. Rename it `presentWithGui` in this class and leave
  `Renderer::renderGui` calling it, or split the present into a private `present()` the two callers
  share. Prefer the split: `renderGui` then means what its base class says it means.

**Verify.**

```
cmake --build build-debug --target components-tests --target openmw
./build-debug/components-tests --gtest_filter='RtxTextureBuilderTest.*:RtxMemoryReportTest.*'
```

---

## What this plan does not close

- **The shaders.** `components/rtx/shaders/` and `components/rtxvulkan/shaders/` were not reviewed,
  so nothing here touches them.
- **`VulkanRenderer::renderFrame` at 295 lines.** It is a frame script and reads top to bottom; the
  three `historyAnswered = true` assignments are the one thing in it a reader has to hold, and the
  code says why they are written where they are read. Leave it until something in it changes.
- **`RtxRenderer`'s size.** Forty members over window, frame, mirror, interface, capture, session and
  report. Splitting it is a change the review did not measure the benefit of. Raise it separately.
- **`components/rtxvulkan/sceneacceleration.cpp` and `scenemicromaps.cpp`**, which hold the two other
  long functions and were not read.

## Landing order and cost

| stage | items closed | risk | frame path touched |
| --- | --- | --- | --- |
| 1 free list | 3 | low, and it is a defect | yes — run the gate |
| 2 digest fields | 3 | low, digest values move | the gate itself |
| 3 `SlotRows` | 3, and two hand-rolled sites | medium, four tables | yes — run the gate |
| 4 `RunBuffer` | 4 | medium, nine sites | yes — run the gate |
| 5 deformer arrivals | 3 | low | yes — run the gate |
| 6 dispatch helpers | 5 | medium, twelve files | yes — `shot` and the gate |
| 7 camera | 1 | low | yes — `shot` |
| 8 mesh resolver | 2 | medium | yes — run the gate |
| 9 material resolver | 1 | low | yes — run the gate |
| 10 casts | 3 | low | yes — run the gate |
| 11 small ones | 5 | low | no |

All thirty-three of the review's items. One of them — the placement change lists, under stage 5 —
is answered with a comment rather than with a change, and the stage says why.

**After every stage**, and before saying it works:

```
CLANG_FORMAT=clang-format-14 CI/check_clang_format.sh
```
