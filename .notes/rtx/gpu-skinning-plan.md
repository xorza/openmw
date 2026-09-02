# Skinning on the device

`gpu skin` is a line in `todo.txt`. The renderer poses every skinned body and every morphed face on
the host, and then compares, copies and sends every vertex to the device, every frame. This plan
moves the pose into a compute pass in front of the refit. The host then sends a few dozen matrices
per actor and no vertices. The shape follows `device-address-plan.md`: what is there, what the field
does, what to build, in what order, what proves it, what could go wrong.

**One upstream file has to gain three accessors** (§3.1). Everything else lands in the RTX places.

## 1. What is there today

### 1.1 The walk poses on the host

`MirrorTraversal` walks the graph every frame. `readDrawable` at `sceneextractor.cpp:962` meets a
`SceneUtil::RigGeometry` or a `SceneUtil::MorphGeometry`, runs `PoseCull` over it, and reads the
geometry the cull wrote. `RigGeometry::cull` in `components/sceneutil/riggeometry.cpp` does the
work: `Skeleton::updateBoneMatrices`, then one blended matrix per group of vertices that share an
influence list, then a transform of every position, every normal and every tangent into one of two
`osg::Geometry` copies.

### 1.2 What the host then does with the pose

`resolveMesh` at `sceneextractor.cpp:1387` hands the posed arrays to `SceneDesc::updateMesh`
(`scenedesc.cpp:181`). That compares every position and normal against the held copy, copies both
where they differ, walks the positions again for the bounds, and names the mesh in `getDeformed`.
The backend writes the positions into this slot's copy of `mPositions`
(`SceneAcceleration::prepareRefit`, `sceneacceleration.cpp:415`) and the normals into this slot's
copy of `mNormalTable` (`SceneBuffers::place`, `scenebuffers.cpp:421`), both through the BAR. The
refit reads the positions. The trace reads the normals.

Per skinned vertex per frame, on the host:

| step | where | bytes read | bytes written |
|---|---|---:|---:|
| blend and transform | `RigGeometry::cull` | 40 | 40 |
| compare | `updateMesh` | 48 | 0 |
| copy | `updateMesh` | 24 | 24 |
| bounds | `boundsOf` | 12 | 0 |
| write to the device | `prepareRefit`, `place` | 24 | 24, across the bus |

The tangent is 16 of the 40 bytes in the first row, computed and written for a reader this renderer
does not have. And per rig per frame, `RigGeometry::cull` allocates a `std::vector<osg::Matrixf>`
for its bone matrices. That allocation sits on this renderer's frame path today, in upstream code
the fork does not edit.

The reference survey (`rtxmw/docs/design.md` §8.9): 556 skinned files, mean 1401 vertices; the
busiest cell places 22 actors; a worst frame deforms about 35 000 vertices. Balmora places nine,
Seyda Neen two. At 35 000 vertices the table above is about 5 MB of host traffic and 0.8 MB across
the bus, per frame.

### 1.3 What the device already has

- The positions of a deforming mesh live in `SlotBlocks mPositions`, one copy per frame slot,
  host-written, with the device address bit. The structure over them is built with `ALLOW_UPDATE`
  and refitted in `recordRefit`. None of that changes.
- The normals live in `SlotBlocks mNormalTable` with the same shape. The shader reads them through
  `GpuTables::mNormalBlocks`.
- Both `SlotBlocks` keep an account of which runs each copy owes. `prepareRefit` pays the account
  with a `memcpy`. The account is exactly the list of dispatches the pass needs.
- `placeScene` waits the fence of the copy it writes (`finishThrough(mReadBy[into])`) and submits
  the placement's command buffer before the frame's trace. A dispatch recorded there runs before the
  refit and is complete before the trace.
- `ComputePipeline` with push constants is the shape of every pass but the trace.
  `buffer_reference` blocks under `scalar` layout are the shape of every table read.
- `Buffer::hostWritten` is device-local memory the host writes through the BAR. A shader reads it
  at memory speed.

### 1.4 What the game states about a rig

`NifOsg::Loader::handleNiGeometry` at `nifloader.cpp:1758` builds a `RigGeometry` from
`NiSkinInstance`: per bone a name, an inverse bind matrix and a bound sphere; per vertex a list of
(bone, weight); one skin transform; a root bone name. `RigGeometry::setInfluences` groups the
vertices by identical weight list. The `InfluenceData` is reference-counted and shared by every copy
of the drawable: `CopyRigVisitor` in `attach.cpp` clones a body part per actor, and the clone
shares the data.

The arithmetic in `cull`, in OSG's row-vector convention (`v' = v · M`):

- `A_i = invBind_i · boneInSkeleton_i` for each bone the skeleton resolved. Zero for a bone it did
  not — that bone's weight is dropped, not renormalised.
- `M = (Σ w_i A_i) · T`, with only the 4×3 affine part summed. `T = skinToSkel · skinTransform`,
  and `skinToSkel` is identity where the pointer is null.
- `p' = p · M`. `n' = n · M_3x3`, not normalised.

`Bone::mMatrixInSkeletonSpace` is public. `Skeleton::updateBoneMatrices(n)` is public and does
nothing for a number it has seen. `mSkinToSkelMatrix` is computed in `updateBounds` under the update
traversal. `InfluenceData`, `mNodes` (the resolved bones) and `mSkinToSkelMatrix` are private with
no accessor.

A skinned geometry is never morphed: `nifloader.cpp:1791` skips the morpher where a skin exists.
The two kinds are exclusive, so a mesh takes one kernel or the other and never both.

### 1.5 Morphs

`MorphGeometry::cull`: `p' = target_0 + Σ_{k ≥ 1} w_k · offset_k`. Positions only. Normals are
never touched. The weights are written by `GeomMorpherController` under the update traversal.
Targets and weights are public. 324 controllers in 273 files: faces.

### 1.6 The doll's pick

`OffscreenTrace::pick` at `offscreentrace.cpp:191` intersects the subject with an
`osgUtil::IntersectionVisitor` at `mPosedFrame`. `RigGeometry::accept(PrimitiveFunctor)` answers
with the geometry the last cull wrote. That is the one consumer of a host-side pose that survives
this plan.

### 1.7 What the change buys

- **Host.** Per actor, a few dozen matrices computed and compared, instead of thousands of vertices
  skinned, compared, copied and written across the bus. The rig's per-frame allocation leaves the
  frame path without an edit to the file it lives in.
- **Device.** A dispatch per posed mesh. The reference measured the whole animated frame — pose,
  refit and top level — at 0.34 ms in a 2.4 ms frame, on the same class of hardware.
- **Not the trace.** It is ray-core bound and the refit is unchanged.
- **Later.** The previous slot's positions are on the device, so a per-vertex motion vector for a
  limb becomes one subtraction at the hit. Today `GpuInstance::mMotion` carries the rigid move only.

## 2. What the field does

- **The reference implementation**, `rtxmw/crates/rtxmw-render/shaders/skin.comp` and
  `skin_pass.rs`: one compute dispatch per placement, 64 lanes, a blend of matrices and not of
  results, bones as three rows of an affine transform, the bind pose read from its own slice and the
  posed result written into the placement's slice, bone matrices written per frame through a mapped
  pointer. Measured (§8.91): refit 0.108 ms against rebuild 0.242 for 22 placements of 1682
  triangles, and the trace the same either way.
- The reference packs four influences per vertex and drops the fifth for 107 of 487 000 vertices.
  This plan does not. The rasterizer applies every influence, and a run per vertex makes that exact
  for the price of one indirection.
- **nvpro-samples `vk_raytracing_tutorial_KHR`**, animation chapter: skin in compute into the vertex
  buffer the structure was built from, then `VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR`, with
  one memory barrier from the compute write to the build's read between them. Quake II RTX skins
  its models the same way.
- **Vulkan.** A build reads its vertex data at
  `VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR` with `VK_ACCESS_2_SHADER_READ_BIT`. A
  compute write before it needs one memory barrier with those as the destination.
- **Upstream OpenMW** has no GPU skinning. There is nothing to lift.

## 3. What to build

### 3.1 One upstream file, three accessors — wait for a go-ahead

`components/sceneutil/riggeometry.hpp`. `InfluenceData` becomes a public nested type. Three
one-line `const` accessors:

| accessor | returns |
|---|---|
| `getInfluenceData()` | `mData.get()` — bones, grouped influences, skin transform, root bone |
| `getBones()` | `std::span<Bone* const>` over `mNodes`; empty until the first update traversal found the skeleton; null for a bone the skeleton has not got |
| `getSkinToSkelMatrix()` | `mSkinToSkelMatrix.get()`; null means identity |

Additions only. No line of the rasterizer's path is edited. The precedent is `getDeformedGeometry`
in the same file, added by the founding commit. The alternative — resolving bone names against the
skeleton on the node path inside the extractor — is a second derivation of
`initFromParentSkeleton`, which the seam rule forbids.

Nothing else upstream. `MorphGeometry` exposes its targets. `Skeleton::updateBoneMatrices` and
`Bone::mMatrixInSkeletonSpace` are public.

### 3.2 The scene description: a rig, a morph, and a pose

`components/rtx/scenedesc.hpp`. `MeshRange::mDeforming` becomes `Deform mDeform` — `None`, `Rig`,
`Morph` — beside `Index mBindOffset`, a run in the bind blocks from a second
`SpanAllocator mBindRuns`, and `Index mRig` or `Index mMorph`.

**`Rig`**, from `addRig`, shared by every mesh built from one `InfluenceData`:

| field | what |
|---|---|
| `mRunOffset` | the rig's first per-vertex run word in `mRuns` |
| `mInfluenceOffset` | the rig's first `GpuInfluence` in `mInfluences` |
| `mBoneCount` | rows one pose of this rig takes |

Per vertex one `uint`: `first << 8 | count`, with `first` relative to `mInfluenceOffset`. Per
influence `{ uint mBone; float mWeight; }`. The extractor flattens `InfluenceData::mInfluences`
into this at first sight. A vertex with no influence has a count of nought and lands at the origin,
which is what the rasterizer's zero matrix does to it.

`addMesh` for a rig takes the **source** geometry — the bind pose. `mPositions` and `mNormals` hold
the bind pose for that mesh from then on, and `getMeshPositions` says so. `writeMesh` is unchanged.

**`poseRig(Index mesh, std::span<const GpuBone> bones, const osg::BoundingBoxf& bounds)`**: asserts
`bones.size() == mBoneCount`; compares the run against `mBones` at the mesh's `mBoneOffset`; where
equal, returns; else copies, sets `mBounds = bounds` and names the mesh in `mDeformed` once.
`mBones` is one host vector of `GpuBone` with a run per rig mesh, allocated at `addMesh`.

**`addMorph(std::span<const osg::Vec3f> offsets, Index targets)`** and
**`poseMorph(Index mesh, std::span<const float> weights, bounds)`**: the same shape.
`mMorphOffsets` is one flat vector, `mWeights` a run per morph mesh.

`updateMesh` goes, and with it the vertex compare and the copy. `getArrivedRigs` and
`getArrivedMorphs` stand beside `getArrivedMeshes` for the backend's extend. `getBones` and
`getWeights` for its place.

**Bounds** come from the drawable: `RigGeometry::getBoundingBox()` is the box `updateBounds` makes
from the bone spheres, in the same space the skinned vertices come out in;
`MorphGeometry::getBoundingBox()` covers every morph. The game's own number, and no walk over the
vertices.

### 3.3 The GPU shapes — `components/rtx/shaders/skinning.h`

| struct | fields | bytes | align |
|---|---|---:|---:|
| `GpuInfluence` | `uint mBone; float mWeight;` | 8 | 4 |
| `GpuBone` | `vec4 mRows[3]` | 48 | 16 |
| `SkinPush` | seven `uint64` — bind positions, bind normals, runs, influences, bones, out positions, out normals — then `uint mCount` | 60, range 64 | 8 |
| `MorphPush` | four `uint64` — base, offsets, weights, out positions — then `uint mCount; uint mTargets` | 40 | 8 |

`GpuBone` row `i` is `(M(0,i), M(1,i), M(2,i), M(3,i))` of the OSG matrix, so the shader's
`dot(mRows[i], vec4(p, 1))` is `p · M`. That is how `InstanceRecord` packs `mMotion` already; one
packing function serves both.

Two kernels in `components/rtxvulkan/shaders/`, each declaring its own `buffer_reference` blocks
under `scalar` layout — `buffer_reference_align` 4 for the float and `uint` blocks, 16 for the bone
block — and no descriptor set:

- **`skin.comp`**, 64 lanes per group. `p = bind[v]`, `n = bindNormal[v]`, `run = runs[v]`.
  `rows = Σ over the run of w · bones[b].mRows`. `out[v] = (dot(rows[0], p1), dot(rows[1], p1),
  dot(rows[2], p1))`. `outNormal[v] = (dot(rows[0].xyz, n), …)` — not normalised, as the
  rasterizer leaves it.
- **`morph.comp`**, 64 lanes. `out[v] = base[v] + Σ_k weights[k] · offsets[k · count + v]`.

Push constants carry the exact slice addresses. `SceneDesc` never lets a run straddle a block, so
one `addressOf(offset)` covers the run, as the refit's `describeTriangles` relies on today.

### 3.4 `SkinPass` — `components/rtxvulkan/skinpass.{hpp,cpp}`

Owns the two pipelines, the bind blocks, the rig and morph tables, one bone buffer and one weight
buffer per frame slot, and the recording.

| member | kind | written |
|---|---|---|
| `mBindPositions`, `mBindNormals` | `BlockedBuffer{VERTEX_BLOCK, 12}` over `mBindRuns` | at arrival |
| `mMorphOffsets` | `BlockedBuffer{VERTEX_BLOCK, 12}` | at arrival |
| `mRuns`, `mInfluences` | `Buffer::hostWritten`, grown through the graveyard | at arrival |
| `mBones[slot]`, `mWeights[slot]` | `Buffer::hostWritten`, grown at arrival | per posed mesh, the slot's copy |

**`extend(scene, graveyard)`**: writes the bind runs of the arrived rig and morph meshes, appends the
arrived rigs' runs and influences and the arrived morphs' offsets, and grows the per-slot bone and
weight buffers to `scene.getBones().size()` and `scene.getWeights().size()`. With nothing in
flight, as every arrival is.

**`record(commands, scene, slot, positions, timer)`**: the account `prepareRefit` pays today drives
it — `positions.write(scene.getDeformed()); positions.sync(slot, fill)`. `fill`, per owed mesh:
write that mesh's bone run (or weights) into `slot`'s buffer; push constants; one
`vkCmdDispatch((count + 63) / 64, 1, 1)`. `mNormalTable`'s own account for deformed meshes goes:
the same dispatch writes both tables. Then one `VkMemoryBarrier2`:
`COMPUTE_SHADER / SHADER_STORAGE_WRITE` → `ACCELERATION_STRUCTURE_BUILD | RAY_TRACING_SHADER |
COMPUTE_SHADER / SHADER_READ`. Zone `"skin"`. Nothing is recorded where nothing is owed.

The write-after-read against the copy's previous reader is the fence `placeScene` already waits.
The read-after-write into the frame's trace is queue order plus the barrier's destination stages.

**`VulkanRenderer::recordPlacement`** order: `release` → `updateInstanceRecords` → **`mSkin->record`**
→ `mAcceleration->place` (refit, top level) → `mBuffers->place`. `prepareRefit` keeps its build
descriptions and loses `mPositions.write` and `sync`: the pass owns that account, and `mPositions`
is handed to it by reference. `mUpdatable` stays. Every posed mesh is built updatable by
construction now, so the "built again" branch for an unmarked deformer goes.

The doll's view scene (`slot != sWorld`) takes the same path with slot 0, inside its
`submitAndWait`.

### 3.5 The extractor

`readDrawable` stops running `PoseCull`. A rig answers `{ rig->getSourceGeometry(), Deform::Rig }`.
A morph answers `{ morph->getSourceGeometry(), Deform::Morph }` with target 0 as the base
positions, because that is what `cull` reads; the loader builds both arrays from the same record,
and an assert says they agree in length.

A rig whose `getBones()` is empty — no skeleton found — is mirrored as a static mesh from its source
and counted in a new `ExtractionStats::mUnskinned`. The rasterizer shows that drawable in its bind
pose too. A canary, like `mSkippedUnknown`.

`resolveMesh` at first sight: `mRigs.try_emplace(rig->getInfluenceData())` → `mScene.addRig(...)`,
flattening the groups into runs through two scratch vectors; then `addMesh(bind, ..., Deform::Rig,
rig)`. Every frame at a known rig: `skeleton->updateBoneMatrices(frame)` with the update traversal's
number, so a skeleton the update already moved answers in one comparison and one it skipped is
brought level; then per bone `B_i = invBind_i · bone_i->mMatrixInSkeletonSpace · T` into
`mBoneScratch`, a zero row for a null bone; then `mScene.poseRig(mesh, mBoneScratch,
rig->getBoundingBox())`. The skeleton is the one `MirrorTraversal::apply(osg::Node&)` already finds
for `markReached`; it stays on the walk's state. `markReached` is unchanged and still what keeps a
semi-active skeleton animating.

Morphs: `poseMorph(mesh, weights of getMorphTargetList()[1..])`.

The existing count check in `resolveMesh` — a rig re-pointed at a longer source — stays and also
compares the rig index.

`ExtractionStats::mDeformed` keeps its meaning: meshes posed this frame.

### 3.6 The pick

`OffscreenTrace::pick` poses first: `PoseCull` at `mTraversals.next()` over the subject, then the
intersection at that number. One host skin per click and none per frame. `PoseCull` stays for this
alone and its comment says so. `MirrorTraversal::mPose` goes.

### 3.7 What goes

- `SceneDesc::updateMesh`, the vertex compare and the copy.
- `prepareRefit`'s position write. `SceneBuffers::place`'s normal write for deformed meshes.
- The rig's per-frame `std::vector`, from this renderer's frame. `riggeometry.cpp` is untouched; it
  stops being called per frame.
- `todo.txt`: the `gpu skin` line.

## 4. In what order

Each step leaves the tree building and the tests passing.

1. **The number before.** `bench --views=balmora --seconds=10` with people on, three rounds; and a
   crowd, `shot --actor=meshes/r/cliffracer.nif` twenty-two times with `--repeat=64` — `mPlace`,
   `mWalk`, the `refit` zone, the frame median and p99. Into `bench.txt`.
2. **The go-ahead on §3.1.** Steps 3 and 4 do not wait for it: the test fixture fills `addRig` by
   hand.
3. **The scene description**, §3.2, with `RtxSceneDescTest` extended. No backend reads it yet.
   `verify --views=all` byte-identical.
4. **The kernels and the pass**, §3.3 and §3.4, proved by `RtxSkinPassTest` reading back. Recorded
   in `recordPlacement`, but no mesh is a rig yet, so the frame is unchanged and `verify` is
   byte-identical.
5. **The extractor and the pick**, §3.5 and §3.6, once §3.1 is in. `updateMesh` and the two host
   writes go. Views without actors stay byte-identical. Views with actors move by rounding, §5.
6. **The number after**, the same runs as step 1, then `todo.txt`.

## 5. What proves it

- **`RtxSceneDescTest`**: `poseRig` with the bones already held names nothing; changed bones name
  the mesh once across two calls; two meshes on one rig each name themselves; `poseMorph` the same;
  `getDeformed` is empty after `clearPlacement`.
- **`RtxSkinPassTest`**, new, `apps/components_tests/rtx/skinpass.cpp`, on the device. The rigged
  quad with one bone at `translate(0, 0, 5)`: the read-back is `(1, 1, 5)` exactly and the normal
  is unchanged exactly. Two bones on one vertex, weights 0.25 and 0.75, translations 4 and 8 in z:
  0.25 · 4 + 0.75 · 8 = 7, exactly. A hand-built rotation with 0 and 1 entries: `(x, y)` →
  `(−y, x)`, and the normal turned the same way, not normalised. A morph: base 0, offset 1 in x,
  weight 0.5 → 0.5 exactly. And the cross-check: the harness's cliff racer posed by the pass
  against the same drawable run through `DeformingCull`, the host path already in `skinning.cpp`,
  every position within 1e-3 units — float rounding on a body a hundred units across, and that is
  the reason for the tolerance.
- **`RtxSceneExtractorTest`** in `skinning.cpp`: the four rig tests re-pointed at what the scene now
  holds. Bind positions in the mesh. Bone rows equal to `invBind · bone · T` hand-computed — for
  `translate(0, 0, 5)`, row 2 is `(0, 0, 1, 5)`. The same mesh across the double buffer. The
  skeleton reached. The unskinned rig counted.
- **`RtxFrameCostTest`**: the wall gains one rig posed with a new translation every frame. Still
  nought allocations.
- **`verify --views=all`**: byte-identical without actors at every step. With actors, after step 5,
  the count of pixels that moved and by how many levels, in `bench.txt`.
- **`spirv-val`** on the two new modules, as the build already does for every module.
- **One `shot` with actors under synchronization validation**, for the barrier.

## 6. What to measure

The runs of step 1 again: `mPlace` and `mWalk` on the host, the `skin` and `refit` zones on the
device, the frame's median and p99, Balmora with people and the twenty-two in a row. Expect the host
place to lose the per-vertex work, the device to gain a `skin` zone of tens of microseconds, and the
trace to stay where it is. Report the numbers that come out, not these.

## 7. What could go wrong

- **A skeleton no update traversal moved this frame** — inactive, or semi-active and not reached —
  keeps its bone matrices. The compare finds nothing and the mesh keeps its pose. That is what the
  rasterizer shows too.
- **`getBones()` is empty until the first update traversal.** The walk runs after it in the game
  and in the harness, and the fixture calls `update(1)` first. Assert on it and name the drawable.
- **`mSkinToSkelMatrix` is recomputed only while the skeleton is active, or on the first frame.**
  The rasterizer's rule, inherited as it stands.
- **The first build is over the bind pose**, where today it is over the first posed frame. A refit
  keeps the tree's shape, so a tree shaped for the bind pose is what every later pose refits into.
  If the `trace` zone on actor views moves, run the pass in `extend`'s batch before `buildMeshes`
  so the first build sees the pose. The pose is known by then: the walk posed before it handed over.
- **`VertexList` is `unsigned short`.** Upstream cannot skin a mesh past 65 535 vertices either.
  Nothing new, and `addMesh`'s block limit already names it.
- **Rounding.** The device sums `Σ w_i (A_i · T)`, the host `(Σ w_i A_i) · T`. Equal in real
  arithmetic, an ulp apart in float. Hits move by a rounding, and a lamp pick can flip on a dark
  pixel, as `device-address-plan.md` §6 saw once. State the count in `bench.txt`.
- **The Metal backend** compiles `skinning.h`. `uint64` and `vec4[3]` align the same in MSL. Its
  kernel is that machine's job and its skipped tests are the result.

## 8. Out of scope

- Per-vertex motion vectors for limbs. This enables it. An item of its own.
- Tangents. The renderer has no normal maps.
- Octahedral normals, `shader-review.md` item 3. A third of the pass's write, worth taking once the
  pass exists.
- A GPU pick for the doll.
- Bone matrices from keyframes on the device. The game's animation is host-side and stays there.

## References

- rtxmw, `crates/rtxmw-render/shaders/skin.comp`, `crates/rtxmw-render/src/skin_pass.rs`,
  `docs/design.md` §8.91 to §8.93
- nvpro-samples, `vk_raytracing_tutorial_KHR`, the animation chapter,
  https://github.com/nvpro-samples/vk_raytracing_tutorial_KHR
- Khronos, `VK_KHR_acceleration_structure`, update mode and the build stage's access masks
- OpenMW, `components/sceneutil/riggeometry.cpp`, `morphgeometry.cpp`, `skeleton.cpp`,
  `components/nifosg/nifloader.cpp:1758`
