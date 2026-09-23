# Stereo and VR for the ray-traced renderer

Design and implementation plan. Status: proposed, nothing built. Date: 2026-09-23.

**Decisions of 2026-09-23:**

- The OpenXR loader is approved as a dependency: the Arch package `openxr`, and for Windows CI the
  OpenXR SDK release.
- The target rate is **90 Hz** (11.1 ms a frame).
- The developer has **no headset**. Development uses a stereo window (phase 3, now required).
- Headset tests run on **a friend's Windows PC with a desktop RTX 3090**, from debug builds that
  the developer sends. Section 8 is that test loop.

## 1. Goal and scope

- Seated VR on a PC headset, first target the Valve Steam Frame through SteamVR.
- Head tracking drives the camera. The gamepad and the keyboard move the player, as now.
- The ray-traced renderer only. The rasterizer keeps upstream's `components/stereo` and does not change.
- Out of scope: motion controllers, hands, a player body, 3D menus. The openmw-vr fork (madsbuvi) did that work over several years. It is a later project.

## 2. The target hardware

| fact | value | source |
|---|---|---|
| panels | 2160 × 2160 per eye, LCD | VideoCardz, VRcompare |
| refresh | 72–144 Hz | VRcompare |
| field of view | up to 110° | VRcompare |
| eye tracking | yes, through `XR_EXT_eye_gaze_interaction` | Steamworks, Custom Engines |
| PC use | streams from a PC through SteamVR ("Foveated Streaming" over a 6 GHz adapter) | Valve, VideoCardz |
| Linux PC as host | not stated by Valve. SteamVR-for-Linux issue #946 asks and has no answer | GitHub |

The machines:

| machine | role | GPU | OS | headset |
|---|---|---|---|---|
| the developer's | builds, stereo window, flat measurements | RTX 4090 Laptop GPU, 150 W (Ada) | Arch Linux | none |
| the friend's | headset tests | desktop RTX 3090 (Ampere) | Windows | to confirm: model, SteamVR version |

The Windows host with SteamVR is also the path Valve supports, so the unclear Linux host support no
longer blocks the plan.

Consequences:

- This renderer needs an RTX card, so it runs on the PC and streams. It cannot run on the headset (Arm, SteamOS).
- For a PC application the headset is an OpenXR device of the SteamVR runtime.
- Valve's foveation extensions (`XR_FB_foveation`, `XR_FB_foveation_vulkan`, `XR_META_foveation_eye_tracked`) control fragment density maps of a rasterizer. They do nothing for a ray tracer. Only the gaze (`XR_EXT_eye_gaze_interaction`) is useful here.
- Foveated Streaming is Valve's video compression. It does not reduce the render cost.

## 3. The budget

Measured on 2026-09-23 on this machine (RTX 4090 Laptop GPU, 150 W), 4K output, `balanced`:
trace 5.2 ms for 2.79 MP traced, Ray Reconstruction (RR) 6.8 ms for 8.29 MP output, other passes 2.7 ms, frame 15.1 ms.

Estimate for two eyes: scale the trace by traced pixels, RR by output pixels, and keep the other passes at 2.7 ms plus 0.6 ms for a second fog volume. These are linear estimates. Phase 2 measures them.

| per-eye output | mode (scale) | traced, both eyes | trace | RR, 2 evaluations | frame | rate |
|---|---|---|---|---|---|---|
| 2160² | balanced (0.58) | 3.14 MP | 5.9 ms | 7.7 ms | ~16.8 ms | ~60 fps |
| 2160² | performance (0.5) | 2.33 MP | 4.3 ms | 7.7 ms | ~15.3 ms | ~65 fps |
| 1728² (80 %) | performance (0.5) | 1.49 MP | 2.8 ms | 4.9 ms | ~11.0 ms | ~91 fps |
| 1512² (70 %) | performance (0.5) | 1.14 MP | 2.1 ms | 3.7 ms | ~9.2 ms | ~109 fps |

Rates used: trace 1.864 ms per traced megapixel (5.2 ms / 2227×1253), RR 0.820 ms per output
megapixel (6.8 ms / 3840×2160).

The target is 90 Hz: 11.1 ms a frame.

- RR cost follows the output pixels. At full panel resolution RR alone takes about 7.7 ms. This is the wall.
- 80 % of the panel with `performance` estimates at 11.0 ms on this laptop: no margin at 90 Hz.
  **The default is 70 % with `performance`** (about 9.2 ms here), and the setting moves it.
- Eye-tracked foveation (phase 7) is the way to full panel resolution.

**The friend's RTX 3090 is not measured, and it differs from this laptop.** It is Ampere: its RT
cores are a generation older, it has no hardware for the thread reorder (the extension is accepted
and does nothing), and its tensor cores are a generation older, which RR runs on. Its cost per pixel
can be higher or lower than the estimate. Phase 2 gives the friend a release build and
`bench --stereo`, and the default scale follows that report.

## 4. What exists in the tree

### 4.1 Upstream stereo (`components/stereo`)

- `Stereo::View` is a `Pose` (position, orientation) and a `FieldOfView` (four angles: left, right, up, down). This is the same shape as OpenXR's `XrPosef` and `XrFovf`.
- `Stereo::Manager::UpdateViewCallback::updateView(left, right)` supplies both eyes each frame. `CustomViewCallback` supplies fixed views from settings.
- Settings `[Stereo] stereo enabled` and `[Stereo View]`: eye resolution, and for each eye an offset in Morrowind units, an orientation and four FOV angles. The defaults describe an HP Reverb G2.
- Only the rasterizer uses the manager (`glrenderer.cpp`). The manager itself is tied to the OSG viewer. The ray tracer can reuse the settings and the `View` types. It does not need the manager.

### 4.2 The ray tracer

| part | file | what matters for stereo |
|---|---|---|
| shared camera | `components/rtx/shaders/camera.h` | `Camera` builds `mForward + mRight*x - mUp*y`, `x`,`y` in [-1, 1]. The frustum is symmetric. `screenOf` inverts it with an orthogonal basis. |
| camera builders | `components/rtx/camera.hpp/.cpp` | `makeCameraFromView(view, verticalFov, w, h, near, far)`, `viewBasisOf`, `cameraAtFieldOfView` |
| frame block | `components/rtx/shaders/visibility.h` | 1320 bytes. Per eye: `mOrigin`, `mCamera`, `mArms`, `mArmsSpread`, `mCameraMotion`, `mPreviousForward/Right/Up`. All other fields describe the world. |
| frame block binding | `lib/bindings.glsl` | one buffer, read as `frame` in every shader |
| per-camera trace state | `components/rtxvulkan/tracechain.hpp` | `TraceChain`: G-buffer, fog volume, sprite bins, denoiser history. One per camera by design. The frame has `mFrame`, pictures have `mView`. |
| per-frame renderer state | `vulkanrenderer.hpp` | per eye: `mFrame`, `mPreviousCamera`, `mUpscaler`, `mSum`, `mTargets`. World: `mWorld`, `mWaves`, `mRipples`, `mFog`, `mSkinPass`, the radiance cache. |
| camera fill-in | `VulkanRenderer::sampleCamera` | jitter, motion vectors from `previous`, the arms' jitter |
| where the game builds the camera | `RtxRenderer::describeTrace` | `makeCameraFromView(view, frame.mEye.mFieldOfView, renderW, renderH, …)` |
| pacing and present | `Rtx::Renderer::awaitFrame`, `pacesFrames`, `presentFrame` | `awaitFrame` runs before input. OpenXR's `xrWaitFrame` belongs exactly there. |
| window output | `presenter.hpp`, `swapchain.cpp` | a blit into a UNORM surface. `TonePass` already writes display-encoded bytes. |
| Vulkan setup | `instance.cpp`, `physicaldevice.cpp`, `device.cpp` | `Instance` takes extra instance extensions (the window's). `PhysicalDevice::select` picks the GPU. `Device` calls `vkCreateDevice`. |
| world scale | `components/misc/constants.hpp` | `Misc::Constants::UnitsPerMeter = 69.99125` |

## 5. Design

### 5.1 The eye camera (core, shared header)

An OpenXR eye has an asymmetric frustum. The tangents of its four angles give the image-plane edges at unit distance.

- Add `vec2 mCenter` to `Shaders::Camera`: the image-plane centre, in units of the half extents. Nought for every camera today.
- `rayAt`: `mForward + mRight * (uv.x + mCenter.x) - mUp * (uv.y + mCenter.y)`. Keep the written association and `precise`.
- `screenOf`: subtract `mCenter` after the division, so it stays the exact inverse.
- `mForward` stays the view axis, perpendicular to the plane. Do not skew it: `screenOf`, `clipDepth` and the fog read it as the axis.
- New builder `makeEyeCamera(const ViewBasis&, const EyeFrustum& tangents, w, h, near, far)`. `mRight = right * (tanR - tanL) / 2`, `mUp = up * (tanU - tanD) / 2`, `mCenter = ((tanR + tanL) / (tanR - tanL), -(tanU + tanD) / (tanU - tanD))`. The sign of `y` follows the rule in `Camera`: `y` runs down the image.
- `mSpreadAngle` from the vertical extent, as now.
- `Camera` grows from 60 to 68 bytes. `VisibilityConstants` holds two of them (`mCamera`, `mArms`),
  so it grows from 1320 to 1336 bytes and `mTables` moves from offset 1168 to 1184, which is still
  eight-aligned. Update the three asserts and every host copy.
- `EyeFrustum` is a core type (four tangents). The core does not name OpenXR.

Tests:

- Host: a hand-computed asymmetric frustum. Corner rays and the centre ray, with exact values.
- Host: `screenOf(rayAt(p))` returns `p` for a grid of pixels, symmetric and asymmetric.
- GPU: `shot --against` a baseline shows every picture the same. With `mCenter` at nought, nothing changes.

### 5.2 The eyes through the seam

The core gets an API-neutral description of the eyes. The host (game side) owns where they come from.

```cpp
namespace Rtx
{
    /// One eye relative to the head: where it stands and what it sees.
    struct EyeView
    {
        osg::Vec3f mOffset;      // world units, head space
        osg::Quat mOrientation;  // head space
        EyeFrustum mFrustum;     // four tangents
    };

    struct EyeViews
    {
        std::array<EyeView, 2> mEyes;
        std::uint32_t mWidth = 0;   // per-eye output extent
        std::uint32_t mHeight = 0;
    };
}
```

Sources of `EyeViews`:

1. **Stereo test mode**, no headset: from `[Stereo] stereo enabled` and `[Stereo View]`, the upstream settings. The offsets are already in Morrowind units.
2. **VR**: from the backend's OpenXR session. `xrLocateViews` at the predicted display time, in the `LOCAL` reference space, with positions converted by `UnitsPerMeter`.

`Rtx::Renderer` gets one query, `std::optional<EyeViews> getEyes() const`. It answers nothing for a flat frame. In VR it answers the views located after `awaitFrame`. The seam `MWRender::Renderer` answers "is this frame stereo" for the game side (camera rules, section 5.6). The rasterizer answers no.

### 5.3 One frame, two eyes (backend)

```
awaitFrame                        (VR: xrWaitFrame + xrBeginFrame, then xrLocateViews)
game update, walk, placement      once: TLAS, refit, skin, waves, ripples, fog tile, radiance cache
for each eye:
    frame block (VisibilityConstants) for this eye
    TraceChain: sprite bin, air, trace, composite
    RR (this eye's feature)
    puffs, bloom
shared: exposure (one histogram over both eyes), sun glare (both eyes' counts added)
for each eye: tone into this eye's target
interface (VR: its own target, section 5.5)
presentFrame                      (VR: copy into the eye swapchains, release, xrEndFrame)
```

Decisions and reasons:

- **One launch per eye, not one launch with a depth of 2.** Every shader reads the frame block as `frame`. One launch for both eyes needs an array and an eye index at every read. Two launches cost one extra launch, a few microseconds. Measure in phase 2. Change only if the measurement asks for it.
- **One `TraceChain` per eye.** `TraceChain` already holds everything one camera writes. A second eye is a second chain, not a new mechanism.
- **One RR feature per eye.** Each eye needs its own history. VR mods that run RR do the same.
- **One exposure for both eyes.** Two exposures can differ, and the eyes then see two brightnesses (binocular rivalry). Build one histogram over both eyes' images.
- **One glare value.** The glare is a wash over the whole view. Add both eyes' counts, and fade one value.
- **The same frame index for both eyes.** Both eyes then draw the same noise sequence. This gives correlated residual noise. Test decorrelated noise in phase 2 and choose by eye.
- **A fog volume per eye first.** The froxels are in camera space. A shared volume over both frustums saves about 0.6 ms. The eyes are 4.4 units apart and a froxel is hundreds of units deep, so the error is small. That is a phase 8 change, after a measurement.
- **Arms.** The flat game draws the arms through `first person field of view`. In VR that trick breaks the depth of the arms. In VR, `mArms` equals the eye camera.

### 5.4 OpenXR in the backend

Requirements and practice (OpenXR specification, the OpenXR tutorial, `hello_xr`):

- **Instance and device through the runtime** (`XR_KHR_vulkan_enable2`):
  - `xrGetVulkanGraphicsRequirements2KHR` gives the Vulkan version range.
  - `xrCreateVulkanInstanceKHR` creates the instance from our create info. `Instance` gets a creation hook beside its window extensions.
  - `xrGetVulkanGraphicsDevice2KHR` names the physical device. `PhysicalDevice::select` checks that device against `DeviceFeatures` and refuses it by name if a feature is missing. It does not pick another GPU.
  - `xrCreateVulkanDeviceKHR` creates the device from our create info. `Device` gets the same hook.
- **Session**: `XrGraphicsBindingVulkan2KHR` with our queue family and queue index. The runtime also submits on that queue inside `xrReleaseSwapchainImage` and `xrEndFrame`. All our submits come from the main thread, so the queue is used by one thread at a time.
- **Session states**: call `xrBeginSession` on `READY` and `xrEndSession` on `STOPPING`. Render only in `SYNCHRONIZED`, `VISIBLE` and `FOCUSED`. On `LOSS_PENDING` and `EXITING`, leave VR cleanly.
- **Swapchains: one per eye**, not one with two array layers. SteamVR issue openvr#1822: with an array swapchain it expects the second layer in `TRANSFER_SRC_OPTIMAL`, against the specification.
- **Format**: `R8G8B8A8_SRGB`. The runtime reads an sRGB format as encoded and a UNORM format as linear. `TonePass` writes encoded bytes, so **copy** them (`vkCmdCopyImage`, size-compatible, bits unchanged) into the sRGB image. A blit would encode them a second time.
- **Layouts**: an acquired image is `COLOR_ATTACHMENT_OPTIMAL`. Move it to `TRANSFER_DST_OPTIMAL` for the copy, and back to `COLOR_ATTACHMENT_OPTIMAL` before `xrReleaseSwapchainImage`.
- **Frame loop**: `xrWaitFrame` in `Renderer::awaitFrame`, then `xrBeginFrame`, then `xrLocateViews` at `predictedDisplayTime`. Then acquire, wait, copy, release per eye, and `xrEndFrame` with one projection layer (and the interface's quad layer) in `presentFrame`.
- **Pacing**: the runtime paces. `pacesFrames` answers yes, and `LatencyPacer` and the window swapchain stand aside in VR. The window can show a mirror of one eye (optional).
- **Extent**: `xrEnumerateViewConfigurationViews` gives `recommendedImageRectWidth`/`Height`. The output extent is that times `[RTX] vr resolution scale` (new setting, 0.7 default, section 3). RR picks the traced extent from the output, as now.
- **A report at session start**, one block in the log: the runtime's name and version, the system
  name, the recommended and the largest extents, both eyes' field-of-view angles and poses (the
  IPD), the refresh rates, and whether `XR_EXT_eye_gaze_interaction` is there. The friend sends this
  block back, and the developer copies the values into `[Stereo View]`, so the stereo window shows
  the friend's headset (section 8).
- **New class**: `XrPresenter` in `components/rtxvulkan`, beside `Presenter`. It owns the instance, the session, the swapchains and the reference space. Only the backend names OpenXR.

### 5.5 The interface

- In VR the GUI draws into its own RGBA target with a transparent background, not over the frame.
- `presentFrame` submits it as an `XrCompositionLayerQuad` about 2 m in front of the player, locked to the `LOCAL` space. The runtime composes it, so the text stays sharp and has correct depth.
- Menus, the loading screen and the main menu submit the quad layer alone, with no projection layer.
- During a cell load the main thread blocks, and frames stop. The runtime then shows its last frame or its own grid. The loading screen as a quad layer covers most of this.

### 5.6 The game side

- VR forces first person. Third person and the dialogue camera are off.
- No view bob and no camera shake in VR. These cause sickness.
- The game camera gives the body: position and yaw. The headset gives the head: pitch, roll, yaw and a small offset. Each eye view is body × head × eye.
- The mouse and the gamepad change only the yaw. The headset owns the pitch.
- A "recenter" action keeps the head's position and yaw at that moment, and applies the inverse
  to every later pose. The runtime's own recenter also moves the `LOCAL` space; the game reads
  the `XrEventDataReferenceSpaceChangePending` event and resets its offset.
- The movement direction follows the head yaw or the body yaw. Make this a setting, as openmw-vr does.

### 5.7 The harness

- `shot --stereo` writes `<place>-left.png` and `<place>-right.png`, and a frame hash per eye.
- `bench --stereo` reports the zones per eye and the frame.
- `repeat` compares both eyes.
- A stereo view uses `[Stereo View]`, so the harness needs no headset.
- `--eye-size=WxH` sets the per-eye output extent, for the Steam Frame numbers without a headset.

## 6. Implementation plan

Each phase ends with `rtx debug test`, `shot --views=all --map --against=<before>` (flat pictures unchanged), `rtx debug repeat --pairs=10` and `rtx debug gate`.

### Phase 0: decisions — done

- The OpenXR loader: approved. Arch `openxr`; the OpenXR SDK release for Windows CI.
- The target rate: 90 Hz.
- The test host: the friend's Windows PC with SteamVR and a desktop RTX 3090 (section 8).
- Still open: Monado on the developer's machine, for OpenXR work without a headset (phase 4). It
  is software to install, so it needs your approval.

### Phase 1: the asymmetric eye camera

- `camera.h`: `mCenter`, `rayAt`, `screenOf`, the size assert.
- `visibility.h`: the two asserts of section 5.1.
- `camera.hpp/.cpp`: `EyeFrustum`, `makeEyeCamera`.
- Every host copy of `Camera`.
- Tests of section 5.1.
- Gate: all flat pictures and frame hashes the same as before.

### Phase 2: two eyes in one frame, no headset

- Core: `EyeView`, `EyeViews`, `Renderer::getEyes`. Test mode from `[Stereo]` and `[Stereo View]`.
- Game side: `describeTrace` builds one or two frame blocks.
- Backend:
  - `EyeChain` (a `TraceChain`, an `Upscaler`, targets, the previous camera), two of them.
  - The frame records the world once and the eyes in turn.
  - One exposure and one glare for both eyes.
- Harness: `shot --stereo`, `bench --stereo`, `--eye-size`.
- Tests:
  - Host: the eye views from the settings, hand-computed.
  - GPU: the left eye of a stereo frame with both eyes at the flat camera equals the flat frame, bit for bit.
  - GPU: both eyes' motion vectors are right after a head turn.
- Measure: the budget table of section 3, at 2160² and 1728² per eye. Also one launch against two, and correlated against decorrelated noise.

### Phase 3: the stereo window (required)

The developer has no headset, so this window is where stereo is seen and checked.

- `[Stereo] stereo enabled` with `[RTX]` shows both eyes side by side in the flat window. Each eye
  keeps its own aspect and is letterboxed in its half. The eyes' extents come from `[Stereo View]`.
- An option swaps the halves (right eye on the left), for cross-eyed free viewing, which shows the
  depth on a flat monitor without glasses.
- The mouse and the keyboard stand in for the head, as in the flat game.
- A `[Stereo View]` profile for the Steam Frame, and later one from the friend's session report
  (section 5.4), so the window shows the fields of view the headset will show.
- The harness writes the same side-by-side picture: `shot --stereo --side-by-side`.
- The GUI draws over the whole window, as a stand-in for the quad layer of phase 6.
- Checks by eye in the window: the depth of the arms, the sky at infinity (no parallax), the water
  and the reflections (a reflection has its own parallax), the puffs, text and markers in the world.

### Phase 4: OpenXR presentation

- Dependency (after phase 0): `find_package(OpenXR)`, and bundle the loader in the AppImage and the portable folder, as the Vulkan loader is.
- `XrPresenter`: instance and device hooks, session, states, swapchains per eye, the copy, the frame loop.
- `[RTX] vr` setting (off by default). VR starts only when it is on. With it off, nothing loads OpenXR.
- Windows CI: fetch the OpenXR SDK release with a pinned version and checksum, as the Vulkan SDK
  is fetched now. Bundle `openxr_loader.dll` in the portable folder.
- Without a headset, the developer's loop is:
  1. Monado with its keyboard-driven simulated headset, if you approve the install. It runs the
     real OpenXR calls (session, swapchains, frame loop) on Linux and shows the composited eyes in
     a window.
  2. Otherwise, a fake of the OpenXR calls in the tests only, and every OpenXR fault found on the
     friend's machine (section 8).
- Then SteamVR on the friend's Windows PC.
- Tests: a GPU test that copies a known tone target into an sRGB image and reads the same bytes back. Session state handling is tested by a fake of the four calls, if the Monado test is not practical in CI.

### Phase 5: head pose into the game

- Section 5.6: first person, no view bob or shake, pitch from the headset, yaw from both, recenter.
- `mArms` equals the eye camera in VR.
- Test on the headset: comfort, scale (a door frame looks about 2 m high), latency.

### Phase 6: interface as a quad layer

- The GUI into its own target, `XrCompositionLayerQuad`.
- Menus and loading screens as a quad layer alone.

### Phase 7: eye-tracked foveation (research)

- The gaze from `XR_EXT_eye_gaze_interaction`.
- RR needs a regular pixel grid, so ray-density foveation inside one grid does not fit RR. Two ways fit:
  1. Two regions per eye: a small high-resolution inset at the gaze and a low-resolution full view. Each region has its own RR feature. The inset is composited over the full view.
  2. Leave RR at a lower output and trace more rays near the gaze (samples per pixel), which RR accepts.
- Measure both. The goal is full panel resolution at 90 Hz.

### Phase 8: optimisations after measurement

- One fog volume for both eyes (about −0.6 ms).
- One launch for both eyes, if phase 2 shows the launch overhead matters.
- `XR_KHR_composition_layer_depth`, if a runtime uses the depth for reprojection. Our depth channel is not a depth-format image, so this needs a pass that writes one.
- Reuse of one eye's shading in the other (stereo reprojection). This is research, and the quality cost needs a test.

## 7. Risks

| risk | effect | mitigation |
|---|---|---|
| RR cost follows output pixels | full panel resolution does not fit 90 Hz | resolution scale; phase 7 foveation; Motion Smoothing at 72 Hz |
| Linux host support for the Steam Frame is not stated | no VR on this machine | phases 1–3 need no headset; Monado for phase 4; a Windows host |
| SteamVR array swapchain bug | validation errors or wrong images | one swapchain per eye |
| the runtime chooses the GPU | on a laptop with a MUX off, the iGPU | refuse by name in `PhysicalDevice::select`, as now |
| cell loads block the main thread | frames stop in the headset | loading screen as a quad layer; the runtime shows its last frame |
| frame time spikes | judder, sickness | the existing rule: uniform frame times. VR makes it stricter |
| new dependency | a build and a release need the loader | bundle it; link it only where `[RTX] vr` exists |
| no headset on the development machine | every headset fault costs a round trip to the friend | the stereo window; Monado if approved; the session report; the test checklist of section 8 |
| the RTX 3090 is not measured | 90 Hz may not hold on it | the friend's `bench --stereo`; the resolution scale setting |
| a debug build turns the validation layers on | it fails or runs without layers on a PC with no Vulkan SDK | phase 4 makes a missing layer a logged warning in the game, or the friend installs the Vulkan SDK |

## 8. Headset tests on the friend's machine

The developer has no headset, so every headset test is a build sent to the friend and a report
sent back. Each round trip costs a day or more, so each build must ask the most it can.

### 8.1 The builds

- CI builds the debug flavour on Windows now, but it uploads only the `package` archive, which is a
  release build. Add a `workflow_dispatch` input that also packages the debug flavour in the same
  portable layout, with the NGX libraries and `openxr_loader.dll`.
- Send both for each round: **the debug build** for the headset (asserts, names, validation if
  the layers are there) and **the release package** for the numbers.
- A debug build turns synchronization validation on by default, and the layers come only with the
  Vulkan SDK. Phase 4 makes a missing layer a warning in the log for the game, not a failure.
  Otherwise the friend installs the Vulkan SDK.
- Each build prints its commit and flavour in the log and the window title, so a report names the
  build it came from.

### 8.2 The friend's setup, once

- NVIDIA driver: the version, in the report.
- SteamVR installed, and set as the OpenXR runtime (SteamVR settings, OpenXR).
- Morrowind's data files, and an `openmw.cfg` from the package's launcher or wizard.
- The headset model and its refresh rate set to 90 Hz in SteamVR.

### 8.3 The runs, in order

1. `openmw-rtxtool info` (release): the device report.
2. Flat baseline (release): `bench --views=seyda-neen-ship,balmora-mages-guild --size=3840x2160
   --upscale=performance --window=false --json=flat.json`.
3. Stereo without the headset (release): `bench --stereo --eye-size=<from the session report>
   --upscale=performance --window=false --json=stereo.json`. This is the 90 Hz answer for the
   RTX 3090.
4. VR (debug): start the game with `[RTX] vr = true` from a save at Seyda Neen. Look around slowly
   for a minute, walk to the customs house, open the inventory and the map, quit.
5. Comfort, by the friend's own judgement: does a door look about 2 m high; does the world stay
   still when the head turns; is the text readable; how often does it judder.

### 8.4 What comes back

- The log of each run, with the session report block (section 5.4).
- `flat.json` and `stereo.json`.
- SteamVR's `vrserver.txt` and `vrcompositor.txt` (Steam's `logs` folder) and a screenshot of
  SteamVR's frame timing graph during run 4.
- Screenshots of the desktop mirror, if something looks wrong.
- Answers to the five comfort questions of run 5.

The developer copies the session report into a `[Stereo View]` profile. From then on the stereo
window shows what the headset shows, and most faults reproduce without a round trip.

## Sources

- [VideoCardz: Valve introduces Steam Frame](https://videocardz.com/newz/valve-introduces-steam-frame-its-new-vr-headset-suppoting-foveated-streaming-and-offering-2160x2160-lcd-panel-per-eye)
- [VRcompare: Steam Frame specification](https://vr-compare.com/headset/steamframe)
- [Steamworks: Steam Frame, Custom Engines](https://partner.steamgames.com/doc/steamframe/engines/custom)
- [SteamVR-for-Linux #946: Steam Frame Linux support](https://github.com/ValveSoftware/SteamVR-for-Linux/issues/946)
- [GamingOnLinux: SteamVR 2.17 for the Steam Frame](https://www.gamingonlinux.com/2026/09/steamvr-2-17-arrives-ready-to-go-for-the-steam-frame/)
- [openvr #1822: SteamVR and a single swapchain with array layers](https://github.com/ValveSoftware/openvr/issues/1822)
- [OpenXR registry: XR_KHR_vulkan_enable2](https://registry.khronos.org/OpenXR/specs/1.0/man/html/XR_KHR_vulkan_enable2.html)
- [OpenXR registry: XrGraphicsBindingVulkan2KHR](https://registry.khronos.org/OpenXR/specs/1.0/man/html/XrGraphicsBindingVulkan2KHR.html)
- [OpenXR-Docs #169: guarantees of XR_KHR_vulkan_enable2](https://github.com/KhronosGroup/OpenXR-Docs/issues/169)
- [OpenXR Tutorial: Graphics (Vulkan)](https://openxr-tutorial.com/linux/vulkan/3-graphics.html)
- [hello_xr: Vulkan graphics plugin](https://github.com/KhronosGroup/OpenXR-SDK-Source/blob/main/src/tests/hello_xr/graphicsplugin_vulkan.cpp)
- [Weier et al.: Foveated Real-Time Ray Tracing for Head-Mounted Displays](https://onlinelibrary.wiley.com/doi/abs/10.1111/cgf.13026)
- [Survey: efficient VR rendering (foveated, stereo)](https://www.sciencedirect.com/science/article/pii/S2096579625000580)
- [Mixed News: VR mod brings DLSS Ray Reconstruction to AAA games](https://mixed-news.com/en/real-vr-mod-dlss-ray-reconstruction/)
- [OpenMW VR (madsbuvi)](https://gitlab.com/madsbuvi/openmw)
