# Two backends

A proposal for restructuring `components/rtx` so that a Metal renderer and the Vulkan one are the
same renderer reaching two APIs, rather than two renderers that happen to draw the same game.

This is phase one only: **the redesign, with no Metal in it.** What a Metal backend then has to
implement is §7, and its plan is a separate document. On acceptance this collapses into `.notes/rtx/plan.md`
§4 and a milestone, and the three-document rule is restored.

## 1. What the code is today

Measured over `components/rtx`, `components/rtxbridge`, `apps/rtxtool` and their tests — 10.2k lines
of renderer and 3.7k of test.

| | lines | |
|---|---|---|
| **Already API-neutral** | ~4,400 | `SceneDesc`, `LightGrid`, `SeaState`, the bridge, the harness's world and views, the tests of all of it |
| **Vulkan plumbing** | ~2,300 | instance, physical device, device, memory, buffer, image, commands, swapchain, shader module, validation, requirements |
| **Vulkan-shaped, carrying shared policy** | ~1,300 | `SceneAcceleration`, `SceneBuffers`, `Texture`, `VisibilityPass` |
| **Pixel tests behind a Vulkan fixture** | 2,090 | `apps/components_tests/rtx/visibilitypass.cpp` |
| **Shader** | 1,438 | `visibility.comp` |

Three things in that table decide the design.

**The neutral core is already the largest part, and it is neutral by accident of good taste rather
than by construction.** `SceneDesc` has no Vulkan in it because the bridge had to be able to build
one without a device; `LightGrid` and `SeaState` are host arithmetic. Nothing stops a Vulkan type
being added to any of them tomorrow, because they sit in a library that links `Vulkan::Vulkan`.

**The 2,090-line pixel test file is the most valuable asset here and it is nailed to Vulkan.** It
asserts hand-computed radiances, mip levels, transmittances and hit counts — every one of which is a
statement about the renderer and none about the API. It reaches the GPU through one function,
`countHits()`, which builds a device, an acceleration structure, a pass and an image inline. A Metal
backend that passes this file unmodified is correct. A Metal backend with its own copy of it is two
renderers.

**The shader is 1,438 lines of which about forty touch an API construct.** The rest — the TMA
spectrum, the caustics, the Henyey-Greenstein phase function, the fog march, the cone-traced mip
selection — is arithmetic on `vec3` and `float`. That ratio is what makes one shader body plausible;
§4 is why it is not automatic.

There is also one fact that will not be true for much longer: **M11 has not landed.** There is no
`apps/openmw/mwrender/rtx/`, no interop, no in-game owner. The renderer has exactly one pass and one
consumer of it. Every milestone from here makes this restructure more expensive, and M8 and M9 are
where the shader roughly triples. This is the cheapest this will ever be.

## 2. Where the seam goes

**Three components, not subdirectories.**

```
components/rtx/          openmw-rtx           links no graphics API
components/rtxvulkan/    openmw-rtx-vulkan    links Vulkan::Vulkan
components/rtxmetal/     openmw-rtx-metal     links Metal, QuartzCore
```

> Since written: `components/rtxbridge` folded into `components/rtx`.
> The seam this section argues for is the graphics API and the linker still enforces it; the OSG
> boundary the fourth row implied was never a link boundary and had the same consumers on both
> sides. Read "the bridge" below as the half of `openmw-rtx` that walks a scene graph.

`.notes/rtx/plan.md` §4 rejected grouping this component into subdirectories, and that argument still holds: a
subdirectory would make every include read `rtx/vulkan/device.hpp` for no gain. But the reason to
split here is not tidiness. **A subdirectory is not a link boundary and a library is.** Three targets
means `openmw-rtx` physically cannot include `vulkan_core.h` — the linker enforces the seam that
`.notes/rtx/plan.md` §4 already calls the one that matters, instead of a convention enforced by whoever is
reading the diff.

Files stay flat inside each, as the convention says. The scene-building half links no graphics API
either once §5.5 lands, which it does not today: `texturebuilder.hpp` includes
`components/rtx/texture.hpp` and drags Vulkan through it.

## 3. What the interface is

Not a hardware abstraction layer. The grain is taken from what the call sites already do rather than
from what a graphics API has in it. Here is `countHits()` and `renderShot()` with the noise removed —
they are the same seven steps:

```
bring up a device
build acceleration structures, tables and textures from a SceneDesc
create the pass
per frame: push constants, dispatch, read the hit count
read the image back
report what it cost
```

So:

```cpp
// components/rtx/renderer.hpp

/// What a backend reports about the scene it took. The harness's summary line, as a struct.
struct SceneStats
{
    std::uint32_t mInstances = 0;
    std::uint32_t mCutoutInstances = 0;
    std::uint64_t mStructureBytes = 0;
    std::uint64_t mTableBytes = 0;
    std::uint32_t mTextureCount = 0;
    std::uint64_t mTextureBytes = 0;
};

/// What one traced frame came to.
struct FrameResult
{
    std::uint32_t mHits = 0;
    double mTraceMs = 0.0;
};

/// One traced image, whichever API produced it.
///
/// Six methods, none of them called more than once per frame. Nothing below this line is
/// abstracted: buffers, images, memory, command buffers, descriptors, pipelines and the swapchain
/// belong to a backend outright and are shared with nothing. An interface over those would be a
/// mini-Vulkan that Metal does not fit, and would put a virtual call inside a frame.
class Renderer
{
public:
    virtual ~Renderer() = default;

    /// Multi-line report for `openmw-rtxtool info`: the device and what it can trace with.
    virtual std::string describeDevice() const = 0;

    /// Builds everything a scene needs, replacing whatever was there.
    ///
    /// `textures` are described rather than loaded: the bridge decodes and the backend uploads,
    /// which is what keeps `openmw-rtx-bridge` free of a graphics API. Its entries are indexed by
    /// the material's texture index, so the order is the scene's.
    virtual void setScene(
        const SceneDesc& scene, std::span<const TextureData> textures, const SeaState& sea) = 0;

    virtual const SceneStats& getSceneStats() const = 0;

    /// Resizes the traced image. Kept by the backend; nothing here allocates per frame.
    virtual void resize(std::uint32_t width, std::uint32_t height) = 0;

    virtual FrameResult renderFrame(const Shaders::VisibilityConstants& camera, std::uint32_t accumulate) = 0;

    /// Copies the traced image into `pixels`, four bytes per pixel, tightly packed.
    ///
    /// An out-parameter because a screenshot loop and a golden-image sweep must not go back to the
    /// allocator for a buffer they already have.
    virtual void readPixels(std::vector<std::uint8_t>& pixels) const = 0;

    /// Shows the traced image. Only for a renderer built against a window, which is asserted.
    virtual void present() = 0;
};

struct RendererOptions
{
    std::filesystem::path mShaderDirectory;
    std::uint32_t mWidth = 1920;
    std::uint32_t mHeight = 1080;

    /// Null for the headless path, which is why the harness works over ssh.
    const NativeWindow* mWindow = nullptr;

    /// Vulkan's layers, Metal's API validation. Backends read what applies to them.
    ValidationOptions mValidation;
};

/// Builds a renderer, or nothing where this machine cannot run the backend asked for.
///
/// **Null and a reason rather than a throw.** Bring-up failure is the one failure a caller always
/// wants to act on — the harness skips its GPU tests, the game keeps its rasterizer — and it is the
/// case that would otherwise force this fork to keep exceptions. The shape matches the one the test
/// harness already uses for the same question.
std::unique_ptr<Renderer> createRenderer(const RendererOptions& options, std::string& reason);
```

### What the Metal side says about that shape

Checked against the macOS 26 SDK on the machine this was written on, rather than from memory. Three
things came back, and only one of them wants the interface changed.

**The instance descriptor maps exactly, and Metal is the simpler of the two.**
`MTLAccelerationStructureInstanceDescriptor` is `transformationMatrix`, `options`, `mask`,
`intersectionFunctionTableOffset`, `accelerationStructureIndex` — and every field of §5.1's
`InstanceRecord` lands on one of them. `mMask` is `mask`. `mCutout` is
`MTLAccelerationStructureInstanceOptionNonOpaque` against `…Opaque`, the same either-or as Vulkan's
force-no-opaque bit. The unconditional two-sidedness is
`MTLAccelerationStructureInstanceOptionDisableTriangleCulling`. `intersectionFunctionTableOffset`
stays zero, because inline `intersection_query` answers the cutout in the kernel exactly as the ray
query does now, rather than through a table of intersection functions.

`mMesh` is the one that improves. Metal's `accelerationStructureIndex` indexes the
`instancedAccelerationStructures` array handed to the instance structure, so building that array in
mesh order makes the record's mesh index the descriptor's field *directly* — where Vulkan needs a
`vkGetAccelerationStructureDeviceAddress` per instance. `Transform3x4` costs a reindex and nothing
more: `MTLPackedFloat4x3` is four columns of three, so `columns[c][r]` is the neutral type's
`mRows[r][c]`, which is what `theVulkanTransformRestatesTheNeutralRowsUnchanged` exists to pin from
the other side.

**M10's G-buffer is shared work, not per-backend work.** `MTLFXTemporalDenoisedScaler` takes colour,
depth, motion, **diffuse albedo, specular albedo, normal and roughness**, with optional specular hit
distance and a reactive mask — which is DLSS Ray Reconstruction's input set under other names. So the
shader that *writes* the G-buffer is one piece of work in the neutral core, and only the handing of
those textures to `MTLFXTemporalDenoisedScaler` or to NGX differs. That is the opposite of what §7's
table implied when it listed upscaling as a per-backend row.

**`resize` is the one method that will have to change, and not yet.** It takes a single extent, and
the target is 1920x1080 internal to 3840x2160 out — two extents that only stop being equal when an
upscaler exists. Left alone until M10 rather than designed for a caller that does not exist; noted
here so it is a known change and not a surprise.

The rest — `setScene` over a span of descriptions, `renderFrame` over pushed constants, `readPixels`,
`present` against a `CAMetalLayer` drawable instead of a swapchain image — needs nothing. Both
backends render offscreen and blit, which `Swapchain` already gives its own reasons for.

`present()` on a headless renderer is a contract the caller broke, so it asserts rather than
returning an error — the harness knows perfectly well whether it opened a window.

The interface is allowed to change. This fork keeps no backward compatibility, so if Metal wants a
shape Vulkan did not suggest, the answer is to change these six methods and rewrite both callers, not
to add a seventh for the second backend.

## 4. The shader

This is the decision that settles whether "symmetrical" is real or a word in a document.

MoltenVK cannot run this renderer — no acceleration structures, no ray query, and the reason it
stalls is structural rather than a matter of waiting. Slang lists Metal as **No** for inline ray
tracing and has an open design issue rather than an implementation. There is no cross-compiler, and
there will not be one on this project's timescale. So the shader body is either written once in
something both compilers accept, or it is written twice and the Metal one is permanently behind.

The forty API-touching lines are not the obstacle. **The obstacle is that GLSL declares its fifteen
buffers as globals and MSL has no globals** — in Metal every resource arrives as a kernel argument,
so all thirty-five functions would need bindings threaded through, and the two bodies diverge in
every signature.

The fix converges them, and it is worth doing on the Vulkan side regardless. `bufferDeviceAddress` is
already a required feature (`requirements.cpp:46`), so `GL_EXT_buffer_reference` costs nothing new.
The fifteen push-descriptor bindings become one struct of addresses carried in the push constants:

```glsl
layout(buffer_reference, scalar) readonly buffer Normals   { vec3 v[]; };
layout(buffer_reference, scalar) readonly buffer Materials { GpuMaterial v[]; };

struct Scene { Normals mNormals; Materials mMaterials; /* ... */ };
```

and the same struct in MSL is

```metal
struct Scene { device const float3* mNormals; device const GpuMaterial* mMaterials; /* ... */ };
```

Both sides read `scene.mMaterials[i]`. Every function takes `Scene scene` — by value in GLSL, where
it is a handful of 64-bit addresses, and `constant Scene&` in MSL. That is a real convergence rather
than a macro standing in for one, and the Vulkan path gets simpler for it: the pass stops writing
fifteen descriptors per dispatch, and the shader stops knowing binding numbers at all. The bindless
texture array still needs a descriptor set, because buffer references do not cover sampled images;
that set stays, and Metal answers it with an argument buffer.

What is left over is the shell, and it is written per backend: the entry point and workgroup
declaration, the ray query calls, texture sampling, the image store, the atomic, and position fetch —
which Vulkan has and Metal answers out of `scene.mPositions`, a pointer the neutral struct already
carries. About two hundred lines each, against twelve hundred shared.

A small `portable.h` covers the rest: `vec3`/`float3`, `mat4x3`/`float4x3`, `inversesqrt`/`rsqrt`,
`out T`/`thread T&`.

**Phase one does the buffer-reference conversion and nothing else about the shader.** It stands on
its own merits, it is a Vulkan-only change verifiable against the existing pixel tests, and it is the
load-bearing prerequisite for a Metal body later. Whether the twelve hundred shared lines really do
compile both ways is a measured spike in phase two — port the wave spectrum and the fog march first,
which are the two densest pieces of arithmetic, and check both compilers agree to the pixel before
committing the rest.

## 5. The plan

Seven steps. Each one leaves the tree building and the suite passing, and each is verified by
`cmake --build build -j32 && ./build/components-tests --gtest_filter='Rtx*'` before the next begins.

### 5.1 Neutral vocabulary, in place

No files move. Add to `components/rtx` the types the seam needs, and take Vulkan out of the ones that
should never have had it:

- `TextureFormat` — an enum of what Morrowind actually ships, replacing `VkFormat` in `TextureData`.
- `Transform3x4` — an affine transform, row-major. `toVulkanTransform` becomes a three-line
  conversion from it in the Vulkan backend; Metal's `MTLPackedFloat4x3` is the other transposition
  from the same type. The hand-computed test that OSG and the GPU move a point to the same place
  moves onto `Transform3x4` and each backend gets a short one that its conversion matches.
- `InstanceRecord` — transform, mesh, mask, and whether traversal must stop for a cutout. **The
  policy that decides these is written once here**: water gets `MASK_WATER` so shadow rays skip it,
  a cutout material gets the no-opaque flag. A backend copies the record into its own instance
  descriptor and decides nothing.
- `SceneStats`, `FrameResult`, `ValidationOptions`, `Backend`.
- `makeCamera` moves out of `visibilitypass.cpp` into `camera.cpp`. It is pure arithmetic that has
  been living in a Vulkan file.
- `Error` stays; `checkVk` and `resultName` are Vulkan's and go with it in the next step.

### 5.2 Split the component

Mechanical. `components/rtxvulkan/` takes instance, physicaldevice, device, requirements, validation,
memory, buffer, image, texture, commands, swapchain, shadermodule, sceneacceleration, scenebuffers,
visibilitypass, compositepass, gbuffer, the `checkVk` half of error, and
`shaders/{visibility.comp, composite.comp}`.

`components/rtx/` keeps scenedesc, lightgrid, wavespectrum, camera, texturedata, instancerecord,
error, renderer.hpp, and `shaders/{scene.h, visibility.h, composite.h}` — which both backends and the
C++ include, and which is why they stay neutral rather than following the GLSL.

`openmw-rtx` drops `Vulkan::Vulkan`. Everything that breaks at that moment is the seam being drawn
for the first time, and the list of breakages is the list of things §5.1 missed.

The shader build moves to `components/rtxvulkan/CMakeLists.txt` — `glslc` and `spirv-val` are Vulkan
toolchain, and a macOS configure must not require them.

### 5.3 `Renderer`, and Vulkan behind it

`VulkanRenderer` absorbs what `renderShot()` and `countHits()` do inline today: instance, physical
device, device, command pool, acceleration structures, tables, textures, the pass, the target image,
the hit-count buffer, the submit, and the readback. Nothing new is written — it is the two call sites
merged, which is why they agree afterwards and why the diff is mostly deletion.

Two properties to hold onto while moving it: the target image and the hit-count buffer are created
once and reused, so `renderFrame` allocates nothing; and `readPixels` fills a caller's vector.

**And this is where the exceptions go.** `createRenderer` returning null with a reason is what gives
bring-up failure somewhere to land, so the twenty-odd `checkVk` throws behind it stop needing to be
caught by anyone. It also deletes `Testing::findInstanceObstacle`: a harness that skips when
`createRenderer` hands back null is asking the same question through the front door, and the test
that builds an instance of its own is asking it because no renderer existed to ask.

### 5.4 The consumers

`shot.cpp`, `view.cpp` and `countHits()` are rewritten against `Renderer`. This is the step the whole
proposal is for.

- `shot.cpp` loses every Vulkan include and becomes: make a renderer, set the scene, trace `mRepeat`
  times, read pixels, write the PNG, print the summary from `SceneStats` and `FrameResult`.
- `view.cpp`'s flight loop — 522 lines of camera, input, screenshots and title-bar instruments —
  becomes backend-neutral, with only the window and its surface staying per backend.
- `countHits()` in the 2,090-line test file becomes four calls on a `Renderer`. **From this point the
  pixel tests are the acceptance suite for any backend**, and the day a Metal one exists they run
  against it with no edit.

`apps/rtxtool/window.hpp` splits: `FlyCamera` is neutral and stays; `Window` and `Surface` go to the
backend that knows what a surface is.

### 5.5 The bridge stops touching the GPU

`buildTextures` currently takes a `Device` and a `CommandPool` and returns a `TextureArray`.
It becomes `describeTextures`, returning the `TextureData` descriptions the scene names, in the order
it names them, with the `osg::Image` references held so the bytes outlive the upload. `describeImage`
already does the hard half and is already neutral.

Nothing that builds a scene then names a graphics API, which is what it always claimed to be.

### 5.6 The scene as a struct of addresses

The buffer-reference conversion from §4, Vulkan only. `SceneBuffers` hands out one `Scene` struct
instead of twelve `VkBuffer` handles; `VisibilityPass` drops its fifteen-binding push-descriptor
set and pushes the addresses; the shader's fifteen `layout(set, binding)` blocks become one struct
threaded through the functions that read it.

The pixel tests are the check: every radiance, mip level and transmittance in that file must come out
unchanged, and any that does not is the conversion getting a binding wrong.

### 5.7 The switch

`OPENMW_RTX_VULKAN` and `OPENMW_RTX_METAL`, defaulting to what the platform has, and one small
`openmw-rtx-backends` that consults them. It exists because the core declares `createRenderer` and
cannot link a backend without a cycle, and because every consumer — the harness, the tests, the game
— wants a renderer rather than a choice.

**And it offers no choice, because no machine has one.** A build has the backend its platform runs
natively and at most one more that it merely compiles; asking for the second could only ever return
the reason it cannot run. So there is no `--backend=` and no `Backend` in `RendererOptions` — which
backend a machine develops is settled by what the machine is, exactly as §7 of `CLAUDE.md` has it.
Comparing the two is diffing images from two machines, not a flag on one.

A build with no backend at all is a hard failure naming it, in the spirit of `.notes/rtx/plan.md` §5:
there is no fallback path, there is a different renderer.

## 6. What this costs, honestly

- **~2,300 lines of Vulkan plumbing get a Metal twin.** Device bring-up, allocation, upload, command
  submission, the swapchain. Irreducible, and no abstraction would reduce it — it would only move it.
- **The window and present path is irreducible and differs in kind.** Vulkan has a swapchain; macOS
  has a `CAMetalLayer`, and the in-game path there cannot use the `VK_KHR_external_memory_fd` design
  in `.notes/rtx/plan.md` §3 at all, because macOS has never had those GL extensions. That is phase two's
  problem and it is the largest one in it.
- **The shader shell is ~200 lines per backend**, plus whatever the spike in §4 finds.
- **Two float paths can disagree.** The pixel tests are what catches it, and this is closer to a
  feature than a risk: the project's own test posture asks for cross-checked implementations, and two
  independent traversals asserting the same hand-computed radiance is exactly that.
- **The interface can ossify around what Vulkan happens to do.** Mitigated by it being six methods
  that name no resource, and by this fork keeping no compatibility: when Metal wants a different
  shape, both callers get rewritten.

**And it is worth doing even if Metal never happens.** It separates a 4,400-line neutral core from
2,300 lines of Vulkan with the linker holding the line, it takes the pixel tests off the Vulkan
fixture, it takes the graphics API out of the bridge, and it replaces fifteen push descriptors with
one address block. None of that is a portability tax.

## 7. What phase two inherits

A Metal backend implements `Renderer` and nothing else. Its work, in the order the milestones would
take it:

| | |
|---|---|
| Device and capability report | `MTLDevice`, `supportsRaytracing`, the M-series generation |
| Allocation and upload | `MTLHeap`, `MTLBuffer`, a blit encoder; the block-compressed upload is the same bytes |
| Acceleration structures | one `MTLPrimitiveAccelerationStructure` per mesh in `instancedAccelerationStructures`, one instance structure over `InstanceRecord`, built on the GPU with nothing standing in for deferred host operations |
| Bindless textures | an argument buffer, Tier 2 |
| Residency | `MTLResidencySet` — Metal 4 makes explicit what Vulkan never asked for |
| The pass | a compute kernel around `intersection_query`, which is the candidate-and-commit loop this shader is already written as |
| Present | `CAMetalLayer`; in-game, an `IOSurface` shared with Apple's GL, because macOS has never had `external_memory_fd` |
| Upscaling | `MTLFXTemporalDenoisedScaler`, fed the G-buffer the neutral core writes |

**Metal 4, not Metal 3.** The SDK carries `MTL4AccelerationStructure.h`, `MTL4ArgumentTable.h` and
`MTL4FXTemporalDenoisedScaler.h` beside their older namesakes, and the target is an M-series chip on
macOS 26. Picking the older API would be choosing a second legacy path on a fork that keeps none.

**The Metal toolchain is a separate download.** `xcrun metal` reports `missing Metal Toolchain` on a
stock Xcode; `xcodebuild -downloadComponent MetalToolchain` is the first step of phase two and the
reason the MSL half of §4's spike could not be measured while this was written.

What Metal does not have, and what each costs: **no opacity micromaps**, so every alpha-cutout
instance stays in the candidate loop — which this renderer already counts and reports, and which
Morrowind's foliage makes a large number. **No shader execution reordering**, which bites at M8 and
not before. **No `ray_tracing_position_fetch`**, answered from the position pointer the neutral
`Scene` struct carries.

And the target is not the same target. `.notes/rtx/plan.md` §5.3 wants 1920x1080 internal to 3840x2160 at 60 on
an Ada-class card; a ten-core M5 is a different budget and phase two should name its own before it
measures anything.
