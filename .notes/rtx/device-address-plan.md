# Set 0 by device address

Item 2 of `shader-review.md`. Set 0 holds 22 bindings. Seventeen of them are storage buffers. The
pass pushes all of them before the fog dispatches and again before the trace. The proposal moves
every table to a device address in the frame block. Set 0 then holds six bindings.

**Not started.** This file is the route: what is there, what the field does, what to build, in
what order, what proves each step, and what could go wrong.

## 1. What is there today

### 1.1 The set

`bindings.h` numbers set 0 and `VisibilityPass::sBindings` declares it. Bindings 1 to 17 are
storage buffers. `pushInputs` at `visibilitypass.cpp:323` fills one `VkWriteDescriptorSet` per
binding and pushes the set. `record` calls it twice a frame when the air is banked. It calls it once
in a room.

| binding | what | type |
|---:|---|---|
| 0 | the acceleration structure | acceleration structure |
| 1 | the hit counter | storage buffer |
| 2 to 4 | the normal, texture coordinate and index block tables | storage buffer |
| 5 to 12 | meshes, instances, materials, layers, masks, lights, light offsets, light indices | storage buffer |
| 13 | the blue-noise tile | storage buffer |
| 14 to 17 | sprites, emitters, sprite tile offsets, sprite tile indices | storage buffer |
| 18 | the frame block | uniform buffer |
| 19, 20 | the wave surface and curvature cascades | combined image sampler, three each |
| 21 | the fog field | combined image sampler |

### 1.2 What already travels by address

The vertex blocks are the precedent. `bindings.glsl` declares `NormalBlock`, `TexCoordBlock` and
`IndexBlock` as `buffer_reference` blocks with `buffer_reference_align = 4`. The shader reads a
block's address out of a table and constructs the reference. The acceleration builder and the
shader binding table also read addresses. `Buffer::getDeviceAddress` and
`SlotTable::getDeviceAddress` exist. `BlockedBuffer` keeps a vector of block addresses and uploads
it as a table.

`RtxProbeTest` in `apps/components_tests/rtx/probe.cpp` proves the address path. It reads one
pattern three ways: through a descriptor, through an address in a push constant, and through an
address out of a block table. It does this on both memory kinds the renderer uses. Every reading
must equal the pattern exactly.

### 1.3 The frame block

`VisibilityConstants` is 980 bytes. `writeConstants` writes it with `vkCmdUpdateBuffer` each frame,
with a barrier before and after. The pass already fills backend facts into the copy it writes:
`mWaveExtent`, `mWaveSlope`, `mWaveCurvature`, `mWaveResolved` and `mLightGrid`. The light grid's
geometry moved from a binding into this block earlier. That move is the model for this one.

### 1.4 The other backend

`components/rtxmetal/shaders/visibility.metal` takes `constant Scene& tables`. `Scene` is a struct
of typed device pointers. Its comment says the Vulkan shader is being brought to the same shape.
This plan is that.

### 1.5 What the change buys

The host records six descriptor writes per push instead of 22. The "a binding the layout declares
was left unwritten" class of mistake goes with the bindings. `VisibilityInputs` stops carrying a
`VkBuffer` handle, and `SceneBuffers` stops handing out twelve of them.

The trace does not get faster. The review measured the trace as ray-core bound. On this hardware a
descriptor read and an address read reach the same load unit. The gain is the host and the
mistakes, not the frame.

## 2. What the field does

- **NVIDIA's ray tracing tutorial** (`vk_raytracing_tutorial_KHR`) keeps an `ObjDesc` row per object
  with `vertexAddress`, `indexAddress` and `materialAddress` as `uint64_t`. The closest-hit shader
  constructs `buffer_reference` blocks from them. That is the shape proposed here.
- **NVIDIA's descriptor guidance** ("Advanced API Performance: Descriptors") says to keep the number
  of descriptor sets in a pipeline layout as low as possible, and names device addresses with
  `GL_EXT_buffer_reference` as the pointer-like path. It ranks push constants as the fastest path
  with no indirection.
- **The Vulkan guide** states that `robustBufferAccess` and `VK_EXT_robustness2` do not bound an
  access through a `PhysicalStorageBuffer` pointer. The shader owns its own bounds.
- **The validation layers** track every live buffer's address range. GPU-assisted validation
  instruments each pointer access against that table when `gpuav_buffer_address_oob` is on. It is on
  by default. `gpuav_force_on_robustness`, which `Instance` already turns on, skips checks that
  robustness covers. A pointer access is not one of them, so those checks stay on.
- **`GL_EXT_buffer_reference`** defaults `buffer_reference_align` to 16. The qualifier states the
  alignment of every address the type is constructed from. It must be a power of two and at least
  the largest scalar in the block. A `uint64_t` converts to and from a reference. A reference block
  can be read from a uniform block.

## 3. What to build

### 3.1 `GpuTables` in `scene.h`, a member of `VisibilityConstants`

One struct of `uint64_t` addresses, declared beside `GpuLightGrid`. The GLSL side spells the type
as `probe.h` does with its `uint64` alias.

| field | what it addresses | row stride |
|---|---|---:|
| `mNormalBlocks` | the table of normal block addresses, this slot's copy | 8 |
| `mTexCoordBlocks` | the table of texture coordinate block addresses | 8 |
| `mIndexBlocks` | the table of index block addresses | 8 |
| `mMeshes` | `GpuMesh` rows | 12 |
| `mInstances` | `GpuInstance` rows, this slot's copy | 60 |
| `mMaterials` | `GpuMaterial` rows, this slot's copy | 68 |
| `mLayers` | `GpuLayer` rows | 48 |
| `mMasks` | mask weights | 4 |
| `mLights` | `GpuLight` rows | 36 |
| `mLightOffsets`, `mLightIndices` | the light grid's two lists | 4 |
| `mBlueNoise` | the blue-noise tile | 4 |
| `mSprites` | `GpuSprite` rows | 56 |
| `mEmitters` | `GpuEmitter` rows | 60 |
| `mSpriteTileOffsets`, `mSpriteTileIndices` | the sprite tiles' two lists | 4 |

Sixteen addresses are 128 bytes. The member sits last in `VisibilityConstants`. Both languages
align a struct that holds a `uint64_t` to eight bytes, so the member lands at offset 984 after four
bytes of padding. A `static_assert` states the offset and the new size of 1112 bytes beside the
existing one.

**Why the frame block and not a second uniform.** The tables that alternate by frame slot change
address every frame. A block updated "only when a table is remade" is updated every frame anyway.
The frame block is already written every frame, with its barriers in place. The pass already fills
backend facts into it. One write, no new state, no new barrier.

**Why not push constants.** The trace pipelines declare no push constants today, and the addresses
would fit in the 256 bytes. On this hardware a uniform block and a push constant both reach a
constant bank. Push constants would add a range to every layout and a second write per push. There
is no gain to pay for that.

### 3.2 `bindings.glsl`: one reference block per table, and accessor functions

Each table gets a `layout(buffer_reference, scalar, buffer_reference_align = N) readonly buffer`
block with one runtime array `at[]`. The alignment is the largest power of two that divides both
the buffer's start and every element access. A buffer's start is at least 16-aligned on this
device. So the rule is:

| alignment | tables |
|---:|---|
| 16 | `GpuLayer` rows, whose stride is 48 and whose two `vec4` sit at 16 and 32 |
| 8 | the three block tables of `uint64_t` |
| 4 | everything else, because the stride or the element is four-aligned only |

A claim larger than the truth is undefined behaviour. A claim smaller than the truth costs the
compiler a wider load where one was possible. The host checks the claim: a debug assert in
`record` tests each address against the alignment its block declares.

Readers call functions, not the block. `bindings.glsl` defines `meshAt(uint)`, `instanceAt(uint)`,
`materialAt(uint)`, `layerAt(uint)`, `maskAt(uint)`, `lightAt(uint)`, `spriteAt(uint)`,
`emitterAt(uint)` and `blueNoiseAt(uint)`. Each constructs the reference from
`frame.mTables` and indexes it. `normalBlockOf`, `texCoordBlockOf` and `indexBlockOf` read the block
table through a reference of its own. `lampsInCell` in `lights.glsl` and the tile walk in
`sprites.glsl` read their two lists the same way.

The sites to rewrite, by file and line as they stand today:

- `lib/traversal.glsl` 115, 116, 132, 256, 257, 495, 496, 529, 559
- `lib/reorder.glsl` 61, 62
- `lib/reproject.glsl` 25
- `lib/texturing.glsl` 173, 174
- `lib/lights.glsl` 42, 217, 342
- `lib/fog.glsl` 636
- `lib/sprites.glsl` 195, 248, 250, 255
- `lib/random.glsl` 105
- `lib/bindings.glsl` 165 to 178, the three block accessors

No `nonuniformEXT` is needed on a pointer access. The decoration belongs to descriptor arrays, and
a reference is not one. State this in `bindings.glsl` so nobody adds it.

Set 0 after the change:

| binding | what |
|---:|---|
| 0 | the acceleration structure |
| 1 | the hit counter |
| 2 | the frame block |
| 3, 4 | the wave surface and curvature cascades |
| 5 | the fog field |

`bindings.h` keeps six constants and `BIND_COUNT` becomes six.

### 3.3 The host

- **`Buffer` caches its address.** The constructor reads `vkGetBufferDeviceAddress` once when the
  usage carries the address bit. `getDeviceAddress` returns the field. A default `Buffer` has an
  address of nought. `record` then makes no driver call per table per frame. `BlockedBuffer` keeps
  its own vector, because it uploads that vector as the block table.
- **Usage bits.** `SceneBuffers::sTableUsage` gains `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`
  and drops `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT`, because no descriptor names these buffers any
  more. `BlockedBuffer::reserve` makes its table with the address bit. The blue-noise upload in
  `VisibilityPass` gets the address bit. The hit counter keeps its storage bit. `DeviceMemory`
  already sets `VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT` from the buffer's usage.
- **`SceneBuffers::describeTables(slot, GpuTables&)`** fills the thirteen fields the scene owns. The
  twelve `VkBuffer` getters go. `SceneAcceleration::getIndexBlocks` returns the address of the
  index block table, and `VisibilityInputs::mIndexBlocks` becomes a `VkDeviceAddress`. Its comment
  about a table remade on growth stays true and stays.
- **`VisibilityPass::record`** fills `described.mTables` from `describeTables`, from `mBlueNoise`
  and from `inputs.mIndexBlocks`. One debug assert says no field is nought. One says each field is
  aligned as its block declares. `pushInputs` writes six descriptors. The comment at the scatter
  dispatch that counts "twenty-three descriptor writes" is rewritten with the new count and the
  reason the set still stays pushed.
- **`VisibilityInputs` sites.** `VulkanRenderer::describeInputs` at `vulkanrenderer.cpp:422` and
  the frame-cost test at `framecost.cpp:76` build one. Both change with the field.
- **`sBindings`** in `visibilitypass.cpp` shrinks to the six rows. The loop over bindings one to the
  frame goes.

### 3.4 The two list merges

`LightGrid::rebuild` and `SpriteTiles::rebuild` each build an offsets list and an indices list from
one counting sort. Each pair becomes one list. The offsets are biased by `cells + 1`, so the list
is its own header: cell `c` owns `at[at[c]] .. at[at[c + 1]]`. The cell count already rides in the
frame block as `mLightGrid.mSize`, and the tile count is derived from the camera in the shader.

- `LightGrid` and `SpriteTiles` expose `getList()` and lose `getOffsets()` and `getIndices()`. The
  counting sort writes the offsets into the head of one vector and the indices after them.
- `SceneBuffers::Tables` loses two buffers per slot. `place` and `binSprites` make one reserve and
  one write per list instead of two.
- `GpuTables` loses two fields and becomes 112 bytes. `VisibilityConstants` becomes 1096 bytes.
- `lampsInCell` returns `uvec2(list.at[index], list.at[index + 1u])` and the callers read
  `lightAt(list.at[i])`. The sprite tile walk changes the same way.
- `apps/components_tests/rtx/lightgrid.cpp` and `spritetiles.cpp` read the two spans today. The
  helper at `lightgrid.cpp:18` and the loops at `spritetiles.cpp:85`, 203 and 233 read the one list
  with the bias.

### 3.5 What replaces the descriptor's safety net

A descriptor carries a size. A pointer does not. Three things stand in for it.

- **GPU-assisted validation** instruments each pointer access against the live address ranges.
  The report names an access that lands in no live buffer or past its end. This also catches a stale
  address, which the core layers cannot see once the handle is gone. `Instance` already turns on
  `gpuav_force_on_robustness`, which leaves these checks on.
- **The graveyard rule** is unchanged. `place` runs before `record`, so every address `record`
  reads names a buffer that is alive. A buffer that `growTo` displaced is buried until the frame's
  fence. `record` states this order in its comment.
- **`VK_EXT_device_fault`** reports the faulting address on a device loss. The renderer requests
  the extension and never reads it today. A follow-up reads `vkGetDeviceFaultInfoEXT` and names the
  table whose range holds the address. That is the debugging story a descriptor's handle used to
  give, and it is out of scope for this item.

## 4. In what order

Each step leaves the tree building and the tests passing. `verify --views=all` runs after each step
and the picture must not move: this change moves no arithmetic.

1. **The device's answer first.** `RtxProbeTest` gains two readings. One reads the address out of a
   uniform block under scalar layout, which is the construct the frame block will use and which
   nothing has asked the device about. One reads a 48-byte row through a reference declared with
   `buffer_reference_align = 16`, which is the `GpuLayer` shape and the one alignment claim above
   four. Both readings must equal the pattern.
2. **Host plumbing that reads nothing new.** `Buffer` caches its address. The usage bits change.
   `describeTables` exists. `GpuTables` sits in `VisibilityConstants` and `record` fills and writes
   it. The shader does not read it yet. The static asserts, `spirv-val` and the two asserts in
   `record` run on the real scene.
3. **The shader reads the addresses.** `bindings.glsl` gains the reference blocks and the accessor
   functions. The reader sites in §3.2 change. Set 0 shrinks to six. `pushInputs`, `sBindings` and
   `bindings.h` shrink with it. The twelve getters go. `RtxSceneTableTest` asserts on addresses.
4. **The two merges** of §3.4, one at a time.
5. **The numbers** of §6, then the item is deleted from `shader-review.md`.

## 5. What proves it

- **`RtxProbeTest`:** the two new readings of step 1, on both memory kinds.
- **`RtxSceneTableTest`** in `visibility/surfaces.cpp:35`: an empty scene fills every field of
  `GpuTables` with a non-zero address, and each address is aligned as its block declares. This is
  the same claim the test makes today through fourteen `VkBuffer` handles.
- **`RtxFrameCostTest`:** a steady frame still allocates nothing. `GpuTables` is a plain struct by
  value and `describeTables` fills an out-parameter, so nothing here reaches the heap.
- **`verify --views=all`:** byte-identical before and after each step.
- **`spirv-val --scalar-block-layout`** already runs on every module in the build.
- **The RTX test binary** in full, and one visibility test under GPU-assisted validation.

## 6. What to measure

Take each number on a warm card, legs interleaved, with the clock and temperature columns checked,
as `CLAUDE.md` says.

- **`bench`** on Balmora, the mages' guild and Seyda Neen: median, p99 and worst frame. Expect no
  change outside noise. The host saves 16 descriptor writes per push.
- **`shot --repeat=32`** on the same views: the `trace` and `air` zone medians. Expect no change
  outside noise. A change in either direction is a finding to explain, not to accept.
- **`compileEvery`** on a cold pipeline cache, before and after. The layout is smaller. Expect no
  change worth keeping.
- **GPU-assisted validation:** the wall time of one `shot` with GPU-AV on, before and after, and
  whether it completes. The instrumentation moves from seventeen storage descriptors to the pointer
  accesses. This is the one cost that can move by an order of magnitude.

## 7. What could go wrong

- **An alignment claim that is false** is undefined behaviour with no message. The host assert
  and the second probe reading pin it. The spec guarantees only the memory requirement's
  alignment for a buffer's start, so the assert tests the real address and not an assumption.
- **`uint64_t` in a uniform block** is new here. `shaderInt64` is already required.
  `spirv-val` checks the layout, and the first probe reading checks the device.
- **A read past a table** is undefined in production today as well. `robustBufferAccess` is off
  outside validation. Under validation the report changes shape and does not go away.
- **The Metal shader** compiles `visibility.h`. Its `VisibilityConstants` gains the same member
  with the same layout, because MSL aligns a `uint64_t` to eight bytes too. That build is checked on
  the machine that runs it.
- **The two dependent loads** at a hit stay two: the block table, then the block. A descriptor
  fetch stood in front of the table load before. The `trace` zone in §6 says whether that changed.
- **`vkCmdUpdateBuffer`** takes up to 65536 bytes. The block is 1112. The frame block stays a
  uniform buffer well under the 16 KB some drivers cap it at.
- **GPU-AV becomes unaffordable again** through the pointer checks. Then the harness gains a switch
  for `gpuav_buffer_address_oob`, off by default and named in `--help`. It does not go silently.

## 8. Out of scope

- The hit counter as an address. The review keeps it a binding. It can follow at any time.
- `VK_EXT_descriptor_buffer`. It answers the same push cost another way and is not needed once set
  0 holds six bindings.
- The Metal backend's `Scene` struct and how it might read `GpuTables`.
- The `GpuInstance::mMotion` side table of item 3, which changes a row and not a binding.

## References

- NVIDIA, *Advanced API Performance: Descriptors*,
  https://developer.nvidia.com/blog/advanced-api-performance-descriptors
- nvpro-samples, `vk_raytracing_tutorial_KHR`, the `ObjDesc` row and its closest-hit shader,
  https://github.com/nvpro-samples/vk_raytracing_tutorial_KHR
- Khronos, `GLSL_EXT_buffer_reference` and `GLSL_EXT_buffer_reference2`,
  https://github.com/KhronosGroup/GLSL/blob/main/extensions/ext/GLSL_EXT_buffer_reference.txt
- Vulkan Guide, *Robustness*, https://docs.vulkan.org/guide/latest/robustness.html
- Vulkan Validation Layers, GPU-assisted validation and its settings,
  https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/main/docs/gpu_validation.md
