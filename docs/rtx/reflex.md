# NVIDIA Reflex in the ray tracing renderer

The design, the plan it was built by, and what the probe and the measurement answered. Section
1 says what Reflex is and what the driver asks of an application. Section 2 says what this tree
did at each of those points before the pacing, by file. Section 3 is the design as built.
Section 4 is the plan, with what each phase found. Section 5 lists what reading could not
answer and what the probe said.

**What the probe settled, and the one decision it forced.** On this machine the driver paces
the surface under `immediate` and `fifo relaxed` and under neither `mailbox` nor `fifo`, on
KDE's Wayland and under XWayland alike. So `[Video] vsync mode = disabled`, which chose mailbox,
now chooses immediate wherever the driver paces it and the player asked for the pacing: the
pacing is the latency a player turning vsync off reaches for, and the driver offers it under
immediate alone. `enabled` stays FIFO, unpaced, because a frame that meets the refresh is that
setting's promise and relaxed FIFO would break it to be paced; `adaptive` is relaxed FIFO and
paced. The swapchain's log line names the mode in force and the modes the driver paces.

1. [What Reflex is, and what the driver asks for](#1-what-reflex-is-and-what-the-driver-asks-for)
2. [What the tree does today](#2-what-the-tree-does-today)
3. [Design](#3-design)
4. [Implementation plan](#4-implementation-plan)
5. [Open questions and risks](#5-open-questions-and-risks)
6. [Sources](#6-sources)

---

## 1. What Reflex is, and what the driver asks for

Reflex is the driver's frame pacing. Without it the CPU runs as far ahead of the GPU as the
swapchain and the frames in flight allow, and a frame's input is sampled that far ahead of
its pixels reaching the screen. With it the driver tells the application when to start each
frame, so the frame's input is sampled as late as it can be and the frame is submitted just as
the GPU is ready for it. Two side effects come with the pacing: a frame-rate limit the driver
enforces at the same point, and a "boost" that holds the GPU at its top clock.

On Vulkan this is `VK_NV_low_latency2`. The installed header and driver (615.71) are at spec
revision 2. It needs `VK_KHR_timeline_semaphore` (Vulkan 1.2 core here) and `VK_KHR_present_id`
with its `presentId` feature. `VK_NV_low_latency` is the older NvAPI-bound form and is not
used. The driver offers all of these on this machine (`vulkaninfo`).

The driver asks for these calls, per swapchain:

| call | what it is | when |
|------|------------|------|
| `VkSwapchainLatencyCreateInfoNV{ latencyModeEnable }` in `VkSwapchainCreateInfoKHR::pNext` | opts the swapchain in | at swapchain creation, every creation |
| `vkSetLatencySleepModeNV(lowLatencyMode, lowLatencyBoost, minimumIntervalUs)` | the mode, and the frame-rate limit the sleep enforces; returns `VkResult` | after each creation, and on a change; not per frame |
| `vkLatencySleepNV(signalSemaphore, value)` then `vkWaitSemaphores` on that value | the sleep: the call returns at once, the wait is the application's; the semaphore must be a timeline semaphore | **exactly once between presents, before input is sampled** |
| `vkSetLatencyMarkerNV(presentID, marker)` | a timestamp the driver keeps per frame | the markers below, each once per frame |
| `VkPresentIdKHR{ pPresentIds }` in `VkPresentInfoKHR::pNext` | which frame this present is | every present |
| `VkLatencySubmissionPresentIdNV{ presentID }` in `VkSubmitInfo2::pNext` | which frame a submit belongs to | every submit of the frame; explicit attribution is a revision 3 promise, a revision 2 driver attributes by order |
| `vkGetLatencyTimingsNV` | the driver's report per frame: the marker timestamps, the driver's, the OS queue's and the GPU's start and end | when a reading is wanted; two-call with `timingCount` in and out |
| `VkLatencySurfaceCapabilitiesNV` chained into `vkGetPhysicalDeviceSurfaceCapabilities2KHR` | which present modes of this surface offer latency mode | once per surface, and again when the present mode changes |

The markers, in the order a frame sets them (`VkLatencyMarkerNV`):

| marker | where the spec puts it |
|--------|------------------------|
| `SIMULATION_START` | the start of the frame's simulation, after the sleep; the first marker of a frame |
| `INPUT_SAMPLE` | just before input is read; between simulation start and end |
| `SIMULATION_END` | the end of the simulation |
| `RENDERSUBMIT_START` | before the first Vulkan call of the frame |
| `RENDERSUBMIT_END` | after the last submit, before the present |
| `PRESENT_START`, `PRESENT_END` | around `vkQueuePresentKHR` |
| `TRIGGER_FLASH` | on a left click, between simulation start and end: the driver draws its flash for a latency analyser |

The `presentID` is any number that strictly increases from wherever it starts. The four
`OUT_OF_BAND` markers and `vkQueueNotifyOutOfBandNV` are for a frame-generation layer and are
not this renderer's.

What the field's integration guides add, beyond the spec:

- The sleep is where the latency is won. Markers alone let the driver measure; the sleep is
  what moves input sampling later. A sleep placed anywhere but before input sampling wins
  nothing.
- Call the sleep and set the markers whether or not the low-latency mode is on. The mode is
  one flag in the sleep-mode info. NvAPI's guide: "It is recommended to call this function
  even when low latency mode is disabled and minimum interval is 0", and "when using this
  function, it must be called exactly once on each frame. If this function is not called,
  after several frames, the driver would fallback to sleep at its less optimal spot."
  Streamline's guide: "`slReflexSleep` and `slPCLSetMarker` must always be called even when
  Reflex Low Latency mode is Off".
- The frame-rate limit belongs to the sleep. `minimumIntervalUs` is the limit, enforced at the
  same point the latency sleep is; an application that keeps a limiter of its own beside it
  sleeps twice, in the wrong place once.
- The sleep mode is set on a change, not per frame: "it is not necessary nor recommended to
  call this too frequently (e.g. every frame), as the settings persist".
- Boost asks for the top clock "even in scenarios where it is idle most of the frame and would
  normally try to save power"; it "can decrease latency in CPU-limited scenarios", at a power
  cost.
- The timings are a ring of the newest frames. NvAPI keeps 64 and wants ninety frames rendered
  before the ring is trusted.

DXVK is the reference implementation of the Vulkan form (`dxvk_presenter.cpp`,
`dxvk_cmdlist.cpp`): one timeline semaphore for the sleep with a counter for its value;
`VkPresentIdKHR` on every present; `VkLatencySubmissionPresentIdNV` on every submit; the sleep
mode applied after each swapchain creation; the markers set through one function that also
returns a host timestamp. Its two pitfalls: a frame counter that is not monotonic confuses the
driver, and the first frames after a swapchain is remade are skipped for markers until the
counter is known to be valid.

---

## 2. What the tree does today

Read beside [`architecture.md`](architecture.md) §11.2 and §11.6. Every claim below was read
off the file it names.

**The loop** (`OMW::Engine::go`, `engine.cpp`): `mClock.advance(measured)`, `advance`,
`frame()`, then `Misc::FrameRateLimiter::limit()`. `frame()` is input (`InputManager::update`),
the Lua sync, the host's `beforeFrame`, scripts, mechanics, physics, world, GUI, then
`eventTraversal`, `describeFrame`, `updateTraversal`, `renderFrame`. The limiter sleeps
**after** the present, and its `getLastFrameDuration()` — the limit exactly where it slept,
the wall where it did not — is the `measured` the next frame opens the clock with, clamped to
200 ms. The limit is `[Video] framerate limit`, read once at start into `Environment`; the
loading screen, the main menu and the window manager make limiters of their own from
`Environment::getFrameRateLimit()` for the frames they drive; the in-game settings window does
not offer the setting, so it never changes while the game runs. When the window is not visible
`frame()` returns false and the loop sleeps five milliseconds and goes round again without a
present.

**The renderer's frame** (`RtxRenderer::renderFrame`): `mPhase.step(Walking)`; the frame with
the world hidden goes straight to `renderGui()`; otherwise the walk and the sweep, the
hand-over (the placement's submit), the pictures inside the interface, the trace's submit, the
GUI's draw and the present. The present is `renderGui` → `Rtx::Renderer::presentFrame` →
`Presenter::present` → `Swapchain::present` → `vkQueuePresentKHR`, on the main thread, with
`VkSwapchainPresentFenceInfoKHR` chained where the device offers present fences.
`Presenter::present` returns false without a present call when the acquire finds the surface
stale, and false after the call when the present answers out-of-date; both mark the presenter
stale for `RtxWindow::fit` to rebuild on a later frame.

**Submits**: every submit is `CommandPool::submitWithDeferred`, the one `vkQueueSubmit2` in the
backend, one `VkSubmitInfo2` on one queue, signalling the device's one timeline. A load's
batches between frames go through it too.

**Frames in flight** (`FrameRing`): two slots, the CPU one frame ahead of the GPU. `collectFrame`
waits only when the ring is full. This is exactly the render queue Reflex trims: the CPU gets
one frame ahead because nothing tells it to wait, and every frame in that queue is latency.

**GUI-only frames** (`Renderer::renderGuiFrame`): a loading screen, a menu cover, a video and a
message box each present from inside a frame, as often as they like, through `renderGui`. Each
is a present the driver counts, with no input sampled and no simulation.

**The seam** (`MWRender::Renderer`, `renderer.hpp`): every member is a question the game asks.
There is no member for "may the next frame begin"; the limiter lives in the engine's loop. The
base holds the resource system, the clock, the camera, the frame stamp and the stats.

**The backend's boundary**: `Rtx::Renderer` in `components/rtx/renderer.hpp` names no Vulkan.
`RendererOptions` carries `mWindow` and `mVerticalSync`; `setVerticalSync` is the one
present-side setting that changes at run time, and it rebuilds the swapchain.
`RtxRenderer` fills `mVerticalSync` from `Settings::video().mVsyncMode` for both hosts, the
played session and the harness, which writes the setting from its `WindowRequest` in
`applyHostedSettings`: a watched window (`view`) keeps the player's setting, a measured run
passes `Disabled`.

**Extensions** (`requirements.cpp`, `device.cpp`, `physicaldevice.cpp`, `instance.cpp`): a
required list and an optional list; `PhysicalDevice::profileOf` records which optional ones the
device offers; `Device` enables those, chains each one's feature struct into the query and then
into the creation, and loads an optional extension's entry points into private members of its
own (`mCmdSetCheckpoint`) — `DeviceFunctions` is the required set. `Instance` asks for
`VK_KHR_get_surface_capabilities2` only beside `VK_KHR_surface_maintenance1`, and loads
instance-level entry points with `vkGetInstanceProcAddr`.

**The swapchain** (`swapchain.cpp`): `create` reads `vkGetPhysicalDeviceSurfaceCapabilitiesKHR`,
builds `VkSwapchainCreateInfoKHR` with no `pNext`, and is called from the constructor and from
`recreate`. `setVerticalSync` changes the present mode and says whether a rebuild is owed;
`Presenter::rebuild` waits idle and calls `recreate`. `presentModeFor` maps `Disabled` to
mailbox, `Adaptive` to relaxed FIFO, `Enabled` to FIFO.

**Waits**: `Timeline` says every wait is the device's, because a wait is where what the queue
has passed changes. That rule is about the queue's clock. The sleep's semaphore is not the
queue's clock: nothing on the queue signals it, and waiting on it changes nothing about what
may be freed. `makeTimelineSemaphore(device, name)` in `handles.hpp` makes one; `sPatience`
and `checkVkWait` bound a wait.

**Headless and stepped runs**: `bench`, `shot`, `check` and `repeat` build no presenter
(`RunSetup::mHeadless`) and step the world by a stated `mStep`. `view` has a window and follows
the wall. `openmw-rtxtool info` stands a renderer up with no window and prints
`describeDevice`.

**Timing rows** (`framespend.hpp`, `benchrecord.cpp`, `session.cpp`): the stretches sum to the
frame and a share stands beside its whole without being summed — `sNotStretches` names the
shares. `Update` is the game's whole gap between two frames, the limiter's sleep inside it.
The session closes each frame's rows from the frame after, with `Update` taken from the
arriving frame's own report.

---

## 3. Design

### 3.1 The rule

**Whenever there is a window and the driver paces the surface, every present is paced: one
sleep before it, the markers around it, an id on it.** `[RTX] reflex` toggles the low-latency
flag and the boost flag inside the sleep mode, and nothing else. The frame-rate limit is handed
to the driver as `minimumIntervalUs`, and the host's limiter stands down on that path. One path
through the frame, with a flag in it, rather than a branch taken on one machine and not
another.

Where there is no window, no extension, no present-id feature, or a present mode the surface
does not pace, nothing sleeps at the driver and the host's limiter paces the frame as it does
today. Both answers reach the game through one call, and the choice is asked every frame, so a
present mode change moves it without anybody remembering to.

### 3.2 Where each call lands

| moment | who | what |
|--------|-----|------|
| top of the loop, before input | `Engine::go` → `MWRender::Renderer::awaitFrame()` → `Rtx::Renderer::awaitFrame()` | a new present id; the sleep and its wait; `SIMULATION_START`, then `INPUT_SAMPLE`. The engine reads input on the next line. |
| the game's work is done | `RtxRenderer::renderFrame`, first thing after `mPhase.step` | `TRIGGER_FLASH` where a left click landed this frame and the flash is on; `SIMULATION_END`; `RENDERSUBMIT_START`. Before the hidden-world early return, so a GUI-only present from here is still a marked frame. The walk is the renderer's CPU work and the placement's submit is the first Vulkan call of the frame, so the boundary is here and not at `updateTraversal`, which animates the game's graph. |
| every submit | `CommandPool::submitWithDeferred` | `VkLatencySubmissionPresentIdNV{ presentID }` chained while the pool holds a non-zero id |
| before the acquire | `Presenter::present` | a sleep still owed (no `awaitFrame` since the last present: a loading screen, a message box) is paid here, with the start markers, so the driver's once-per-present rule holds at a worse spot |
| before `vkQueuePresentKHR` | `Presenter::present` | `RENDERSUBMIT_END`, `PRESENT_START` |
| `vkQueuePresentKHR` | `Swapchain::present` | `VkPresentIdKHR{ presentID }` chained beside the present fence |
| after it returns | `Presenter::present` | `PRESENT_END`; the frame is over, a sleep is owed; the newest timings read |

The present id opens with the sleep and closes with the present call, whatever the call
answers. The presenter's own count starts at one and never goes back: not across a swapchain
rebuild, not across a resize. The first present after a rebuild carries markers like any
other; the counter never went backwards, so the driver has nothing to recover from.

### 3.3 The pacer's turns

The pacer keeps its turn in `Turn`, and a call out of its turn does nothing rather than
asserting — a window hidden or a present that failed is a turn the frame skips, not a
contract broken. Only the two halves of one present assert their pairing:

| turn | meaning | `awaitFrame` | `endSimulation` | `present` |
|------|---------|--------------|-----------------|-----------|
| `Owed` | a present happened, or none ever has; the next frame owes its sleep | sleep, new id, `SIMULATION_START`, `INPUT_SAMPLE` → `Slept` | out of turn | pays the sleep and sets the start markers first, then as `Submitting` |
| `Slept` | the frame is open, the game is simulating | nothing: the window was hidden or the present failed, and the frame is still open. No second sleep between two presents. | `SIMULATION_END`, `RENDERSUBMIT_START` → `Submitting` | `SIMULATION_END`, `RENDERSUBMIT_START`, then as `Submitting` |
| `Submitting` | the renderer is recording and submitting | nothing, as above | nothing: a second `renderFrame` before a present | `RENDERSUBMIT_END`, `PRESENT_START`, the call, `PRESENT_END` → `Owed` |

A present whose acquire found the surface stale never reaches the call: the pacer stays
where it was, and the frame closes on the next present that does. A present that made the call
and was answered out-of-date is a present the driver saw, and closes the frame.

### 3.4 The seam

`MWRender::Renderer` gains one question and one setting:

```cpp
/// Holds the game until the next frame may begin, and says how long the last one stood for on
/// the wall. The frame-rate limit lives here, and so does whatever pacing a renderer has beyond
/// it: a driver that says when to start the frame answers this. Once per loop, before input is
/// read, because what is read after this is what the frame shows. The rasterizer's answer is
/// the limiter the engine's loop used to hold, in the same place in the cycle.
virtual std::chrono::steady_clock::duration awaitFrame();

/// `[Video] framerate limit`, in frames a second, or nought for none. Once, at start: the
/// setting is the launcher's and is not offered while the game runs.
void setFrameRateLimit(float limit);
```

The base holds the `Misc::FrameRateLimiter` that `Engine::go` holds today. Its `awaitFrame` is
`limit()` then `getLastFrameDuration()`. `Engine::go` becomes:

```cpp
while (!mStateManager->hasQuitRequest())
{
    const std::chrono::steady_clock::duration stood = mRenderer->awaitFrame();
    const double measured = duration<double>(std::min(stood, maxSimulationInterval)).count();
    mClock.advance(measured);
    ...                                  // advance, frame(), the clock's step — as today
}                                        // no limit() at the end
```

`Engine` hands the limit over where it hands the clock over (`setFrameClock`).
`Environment::getFrameRateLimit` stays for the loading screens' own limiters.

`RtxRenderer::awaitFrame`:

```cpp
if (!mRenderer->pacesFrames())
    return Renderer::awaitFrame();       // the base limiter, as the rasterizer has it

const auto began = std::chrono::steady_clock::now();
mRenderer->awaitFrame();                 // the driver's sleep, the frame's start markers
const auto opened = std::chrono::steady_clock::now();
mSleptMs = Rtx::since(began, opened);    // the Sleep row of the frame about to be reported
const auto stood = opened - mOpened;     // from the last frame's opening to this one's
mOpened = opened;
return stood;
```

The two paths measure the same interval — one opening to the next, the sleep inside it — and
differ only in the limiter's rounding to the limit on a frame it slept. When the pacer goes
dormant mid-run (a present mode the surface does not pace), the base limiter's last
measurement is stale by however long the driver paced; the loop's 200 ms clamp bounds that one
frame and the limiter does not sleep on it, because it is already late.

`RtxRenderer::renderFrame` calls the backend's `endSimulation()` first thing, after the phase
step and before the hidden-world return, with the click read off `SDL_GetMouseState` — the
state the frame's own input pump left, so the flash lands in the frame that processed the
click — and only under `[RTX] reflex flash`. No new seam member for the click.

### 3.5 The core: `Rtx::Renderer`

API-neutral, in `components/rtx/renderer.hpp`:

```cpp
/// How the driver paces the frame: off leaves the sleep a frame limiter and the markers a
/// measurement; on asks for the low-latency mode; boost asks for the top clock beside it.
enum class LatencyMode { Off, On, Boost };
inline constexpr NamedEnum sLatencyModeNames{ std::array{ pair{ Off, "off" }, pair{ On, "on" }, pair{ Boost, "boost" } } };

/// What a present paces the frame by, beside the vertical sync: the mode, and the shortest
/// interval between two presents the driver holds the frame to — the frame-rate limit, in
/// microseconds, nought for none.
struct Pacing
{
    LatencyMode mMode = LatencyMode::Off;
    std::uint32_t mMinimumIntervalUs = 0;
};

// RendererOptions gains `Pacing mPacing;` beside `mVerticalSync`.

/// Whether presents are paced by the driver: a window, a driver that paces, and a present
/// mode the surface paces under. Asked every frame; a renderer that answers no is paced by
/// whoever calls it.
virtual bool pacesFrames() const = 0;

/// The pacing, changed while the frames run: a menu change, like `setVerticalSync`. No rebuild:
/// the swapchain is opted in whenever the driver paces.
virtual void setPacing(const Pacing& pacing) = 0;

/// Sleeps until the driver says the next frame may begin, and marks the frame's start and its
/// input sample. Once per present, before input; nothing where `pacesFrames` is false, and
/// nothing on a frame still open.
virtual void awaitFrame() = 0;

/// The game's work for the frame is done and the renderer's begins.
/// @param flash whether the frame is to carry the analyser's flash.
virtual void endSimulation(bool flash) = 0;

/// The driver's timings of the newest finished frame, or nothing where none is: what the
/// title prints and the run's report keeps.
virtual std::optional<LatencyReport> describeLatency() const = 0;
```

`LatencyReport` (`components/rtx/latencyreport.hpp`, its own file) is the report's neutral
shape, in microseconds: `mPresentId`; `mInputToPresentUs` (input sample to present end);
`mSimulationUs`; `mRenderSubmitUs`; `mGpuUs` (`gpuRenderStart` to `gpuRenderEnd`); and
`mSleptUs`, what the last sleep's wait cost the host, measured by the host and not the driver.

`FrameSpend` gains `Timing::Sleep`: what `awaitFrame` cost, a share of `Update` and named in
`sNotStretches` beside `Wait` and `Fold`, so the frame still closes. `Update` keeps the whole
gap, as the doc on `Timing` says it does today with the limiter's sleep inside it; `session.cpp`
closes `Sleep` from the arriving frame beside `Update`. The `Timing` doc names the new share.

### 3.6 The backend

**`Device`** (`device.cpp`): `VK_KHR_present_id` and `VK_NV_low_latency2` join the optional list
in `requirements.cpp`, each with its reason. `VkPhysicalDevicePresentIdFeaturesKHR` is chained
beside the fault and present-fence features and `presentId` enabled where offered. The five
entry points are loaded into a `LatencyFunctions` table the device owns, null where either
extension is absent:

```cpp
struct LatencyFunctions
{
    PFN_vkSetLatencySleepModeNV mSetSleepMode = nullptr;
    PFN_vkLatencySleepNV mSleep = nullptr;
    PFN_vkSetLatencyMarkerNV mSetMarker = nullptr;
    PFN_vkGetLatencyTimingsNV mGetTimings = nullptr;
    PFN_vkWaitSemaphores mWaitSemaphores = nullptr;   // core, in the table so a test can stand in for it
};
bool hasLatencyPacing() const;        // both extensions and the feature
const LatencyFunctions& getLatencyFunctions() const;
```

**`Instance`**: `VK_KHR_get_surface_capabilities2` is asked for whenever there is a surface and
the driver has it, and `vkGetPhysicalDeviceSurfaceCapabilities2KHR` is loaded with
`vkGetInstanceProcAddr` beside the debug messenger's functions; surface maintenance keeps its
own condition.

**`LatencyPacer`** (`components/rtxvulkan/latencypacer.hpp`, `.cpp`), owned by `Presenter`,
because everything it does is per swapchain:

- made from the device's `LatencyFunctions`, the device handle and the sleep semaphore the
  presenter made with `makeTimelineSemaphore`; told the swapchain handle and whether the
  surface paces its mode after every `create` (`follow(VkSwapchainKHR, bool)`), which is
  where it applies the sleep mode. A `vkSetLatencySleepModeNV` that fails is logged once and makes the
  pacer dormant rather than a throw: the frame still presents;
- `isLive()`: the answer `pacesFrames` forwards;
- the turn of 3.3, the present id, the current `Pacing`, the sleep counter;
- `awaitFrame()`: `vkLatencySleepNV` then `mWaitSemaphores` under `sPatience`; the wait's
  length kept for the report;
- `endSimulation(flash)`, `beforePresent()`, `afterPresent()`: the markers by the turn;
- `readTimings()` after each present: `vkGetLatencyTimingsNV` into a fixed
  `std::array<VkLatencyTimingsFrameReportNV, 64>` kept on the pacer, the `sType`s stamped
  before every read because the layers check every entry on the way in, never allocated on
  the frame; the entry with the highest `presentID` becomes the
  `LatencyReport`.

**`Swapchain`**: the constructor takes whether the swapchain is paced and `create` chains
`VkSwapchainLatencyCreateInfoNV{ VK_TRUE }` when it is. `present` takes the present id and chains
`VkPresentIdKHR` beside the present fence when it is non-zero. `Presenter` reads
`VkLatencySurfaceCapabilitiesNV` for the surface once, in its constructor (`PacedModes`,
`pacedmodes.hpp`), because the answer is the surface's and not the swapchain's; the swapchain
says whether the mode in force is on the list, and the presenter tells the pacer.

**`CommandPool`** gains `setPresentId(std::uint64_t)`; `submitWithDeferred` chains
`VkLatencySubmissionPresentIdNV` while the id is non-zero. `VulkanRenderer` sets it from the
pacer after each `awaitFrame` and after each present, so a load's batch between two presents
carries the frame it will be presented under. The pool stays ignorant of the presenter.

**`VulkanRenderer`**: `pacesFrames` is "the presenter has a live pacer"; `awaitFrame`,
`endSimulation`, `setPacing` and `describeLatency` forward to it and answer nothing without
one. `describeDevice` prints the two extensions and the feature; the presenter logs the
surface's paced modes at start beside the swapchain's own line.

### 3.7 Settings, menus, the harness

`[RTX] reflex = on` (`off`, `on`, `boost`) and `[RTX] reflex flash = false`, in
`settings-default.cfg` with their reasons and in `Settings::RTXCategory`. `on` by default: the
pacing costs nothing a player would give up, and boost is a power decision a laptop's owner
makes. `RtxRenderer` fills `RendererOptions::mPacing` from `Settings::rtx()` and
`Settings::video().mFramerateLimit` the way it fills `mVerticalSync`, for both hosts.
`processChangedSettings` maps a change of `[RTX] reflex` to `setPacing`.

The harness's `WindowRequest` gains `Rtx::LatencyMode mLatency` beside `mVerticalSync`, filled
by the same rule: a watched window keeps the player's setting, a measured run passes `Off`;
`applyHostedSettings` writes it into `Settings::rtx()`. A measured run with a window is still
paced by the sleep at `Off` — a limiter with no limit and a set of markers — which phase 0
measures to be a wait of nothing.

The settings window's ray tracing page gains a combo box in the order `sLatencyModeNames`
states, wired as the upscale box is (`SettingsWindow::onRayTracingUpscaleChanged` is the
pattern); the launcher's graphics page the same; the strings are `OMWEngine:RayTracingReflex*`
in `files/data/l10n/OMWEngine/*.yaml` and the launcher's `.ts` files. A machine whose driver
does not pace shows the box and logs once that the setting had nothing to move, the way an
unreachable upscale mode does.

### 3.8 Reporting

- The window title (`FrameTimer::addFrame`, 96 bytes) gains the newest `mInputToPresentUs`
  once a second beside the frame rate: `"… 143 fps, 11.8 ms"`.
- `FrameReport` carries `std::optional<Rtx::LatencyReport> mLatency`; headless runs carry
  nothing, and the JSON says so by absence. `compare` treats an absent key as absent on both
  sides, which is what it does for any key today and phase 3 asserts.
- `view --frames=N` prints the last report's latency line where it prints where it stood.

### 3.9 What does not change

- The picture. The sleep delays the host and moves nothing a shader reads: the clock, the
  frame index, the jitter, the scene. `repeat` cannot see it, and it runs headless anyway.
- The ring. Two slots and the CPU one ahead stay as they are; under the pacing the CPU arrives
  when the GPU is nearly free, so `collectFrame` finds the frame behind finished more often.
  `FrameResult::mInFlight` will read one more often in a paced window; `frames-overlap` stays a
  headless claim.
- Threads. Every call is on the main thread, where the present already is.
- The rasterizer. It sees `awaitFrame`'s default and the limiter it had.
- The loading screens. Their limiters and their presents stay; each present pays its sleep at
  the presenter.

---

## 4. Implementation plan

Each phase builds, is tested and passes the gate on its own. The order puts the probe first,
because the one thing reading cannot answer is whether the driver paces this window on this
compositor.

### Phase 0 — Probe (half a day) — done

Answer 5.1 before the design is committed to. **Answered:** the surface's paced modes are
`immediate` and `fifo relaxed`, under Wayland and XWayland both; the sleep waits and the
timings fill under `immediate`. One thing the spec does not say and the layers do:
`vkGetLatencyTimingsNV` writes its entries whole, so every entry's `sType` is set again before
every read. No throwaway branch was needed: the `Sleep` row and the `latency ms` line of a
`view --frames=300` report are the probe.

1. `requirements.cpp`: `VK_KHR_present_id` and `VK_NV_low_latency2` join the optional list, with
   the reason each is there. `Device` chains and enables `presentId`, loads `LatencyFunctions`,
   answers `hasLatencyPacing`. `describeDevice` prints both and the feature.
2. `Instance` asks for `VK_KHR_get_surface_capabilities2` with any surface and loads
   `vkGetPhysicalDeviceSurfaceCapabilities2KHR`. `Presenter` reads
   `VkLatencySurfaceCapabilitiesNV` and logs the paced modes beside the swapchain line.
3. A throwaway branch of `Presenter::present`: opt the swapchain in, set the mode with
   `lowLatencyMode` on, sleep before the acquire, and log the wait's length and the driver's
   timings for the first hundred frames of a `view`.

Verify: `info` names the extensions and the feature on this machine; the `view` log names at
least one paced mode for the surface, shows a non-zero wait, and timings whose
`gpuRenderStartTimeUs` is non-zero. If the surface names no paced mode or the timings stay
zero under Wayland, run the same under `SDL_VIDEODRIVER=x11` and record which compositor paces
(5.1). Then the same with the mode off and the interval nought, to see the wait read nothing
(3.7). Tests: `RtxRequirementsTest` gains the two names; the feature-table test proves the new
accessor addresses a distinct field. The throwaway branch is removed before the gate.

### Phase 1 — The seam and the core (one day) — done

1. `MWRender::Renderer::awaitFrame` and `setFrameRateLimit`; the limiter moves from
   `Engine::go` into the seam's base; `Engine::go` opens the clock with what `awaitFrame`
   returns; `Engine` hands the limit over beside the clock.
2. `Rtx::LatencyMode`, `sLatencyModeNames`, `Pacing`, `LatencyReport`;
   `RendererOptions::mPacing`; the five virtuals on `Rtx::Renderer`, answered by
   `VulkanRenderer` as nothing for now; `Timing::Sleep` with its share rule in `sNotStretches`
   and its close in `session.cpp`.
3. `[RTX] reflex` and `[RTX] reflex flash` in `Settings::RTXCategory` and
   `settings-default.cfg`, with their reasons; the menus wait for phase 3.
4. `RtxRenderer::awaitFrame`, `endSimulation` at `renderFrame`'s entry, `mSleptMs` into the
   report; `RtxRenderer` fills `mPacing` from the settings; `processChangedSettings` →
   `setPacing`; the harness's `WindowRequest::mLatency`.

Tests: `RendererTest` proves the base `awaitFrame` sleeps to the limit and answers the
duration — a limit of 200 Hz, two calls in a row, the second answers 5 ms exactly, because
the limiter answers the limit where it slept; and with no limit answers the wall.
`sLatencyModeNames` round-trips its three names and refuses a fourth. `benchrecord.cpp`'s
row test covers `Sleep` as a share. Gate.

### Phase 2 — The pacer (one to two days) — done

Built as 3.6 says, with one addition the probe forced: the present mode is chosen with the
pacing in mind (`Swapchain::setPreferPaced`), so `setPacing` costs a rebuild where the mode
moves, as `setVerticalSync` does.

1. `LatencyPacer` as 3.6 says, the turn of 3.3 asserted with `Stepped`.
2. `Presenter` owns one where the device qualifies and the surface paces the mode; `present`
   pays an owed sleep before the acquire, sets the markers around the call, tells the pool the
   next id, reads the timings; `Swapchain::create` chains the opt-in; `Swapchain::present`
   chains the present id.
3. `CommandPool::setPresentId` and the chained `VkLatencySubmissionPresentIdNV`.
4. `VulkanRenderer` forwards the five calls; `pacesFrames` answers from the presenter.
5. `RtxRenderer::awaitFrame` takes the paced path.

Tests: the pacer's turns, its ids and its markers are host logic and get a components test
around a `LatencyFunctions` table of stand-ins that record each call — the sleep owed after a
present and paid by the next `awaitFrame` or by the next present; no second sleep between two
presents; the id strictly increasing across a `follow` of a new swapchain; the marker order
per frame; nothing set before the first sleep; a stand-in `mWaitSemaphores` that answers at
once. The GPU tests run headless and cannot open the extension; the window path is exercised by
a windowed smoke run the gate gains as one line — `view --frames=30` under the layers — and by
the log's paced-modes line. Gate.

### Phase 3 — Settings, menus, reporting (one day) — done

1. The settings window's combo box for `[RTX] reflex`; the launcher's; the strings and the
   `.ts` files.
2. The title, the `view` line, `FrameReport::mLatency`, the JSON, and `compare` asserted on an
   absent key.
3. The flash from `SDL_GetMouseState` at `endSimulation`, gated by the setting.

Tests: the settings round trip through `RtxRenderer`'s reading of them (the played setup's
test, if there is one; else `SettingsWindow`'s index translation as the upscale box has); the
title string with and without a report. Gate, then a `view` session with the flash on to see
the square.

### Phase 4 — Measure (half a day) — done

Not a build step; the answer to whether any of this was worth it. **Measured, in
`.notes/reflex-measurements.md`:** at the ship at Seyda Neen, 2000 frames each, the host's wait
for the frame behind went from 2.07 ms a frame after the input to nought, and the driver holds
the host 1.66 ms before the input instead; the frame rate is unchanged at about 206 fps, and
the driver's own input-to-present figure is 3.2 ms under a 4.9 ms frame. Boost bought nothing
at a place that keeps the card busy. The default stays `on`. `--reflex` on the harness's line
is what varied the mode without touching the player's settings.

1. `view --frames=2000` at one place, three runs: `reflex = off`, `on`, `boost`. Record the
   driver's `mInputToPresentUs` median, p99 and worst, `Timing::Sleep`, `FrameResult::mInFlight`,
   and the frame rate. Under vsync off (mailbox) and on (FIFO).
2. The same with the frame-rate limit at the monitor's refresh, to see the driver's limiter
   against the host's.
3. The first frames after a door, for 5.3.
4. Write the figures into `.notes/` beside the other measurements; the setting's default stays
   `on` only if `on` beats `off` on the median and the p99 both.

---

## 5. Open questions and risks

### 5.1 Does the driver pace a Wayland window?

**Yes, under two of the four present modes.** `VkLatencySurfaceCapabilitiesNV` names
`immediate` and `fifo relaxed` for this surface, on KDE's Wayland and under XWayland alike, and
neither `mailbox` nor `fifo`. Under `immediate` the sleep waits and the timings fill; under
`mailbox` the pacer is dormant and the host paces. That is what moved the present mode choice
(the note at the top). Whether the compositor tears under `immediate` is the compositor's:
KDE composites without tearing unless told to allow it, so a windowed game sees no torn frame.

### 5.2 Revision 2 and submit attribution

The installed header and driver report revision 2. Revision 3 (April 2026) is what promises
that `VkLatencySubmissionPresentIdNV` attributes a submit; under revision 2 the driver
attributes by the order of submits between markers. Every submit of a frame here lands between
that frame's `RENDERSUBMIT_START` and `PRESENT_START` on one queue, so implicit attribution
holds; the struct is chained anyway, as DXVK does, and costs nothing.

### 5.3 GUI-only presents

A loading screen presents dozens of times inside one engine frame. Each present pays a sleep
just before itself and sets a degenerate marker set. The driver sees a frame with no
simulation and a zero-length render submit, which is the truth. What it does with the latency
estimate across such a run of frames is unmeasured; the first game frame after a load may be
paced oddly for a frame or two. Phase 4 watches the first frames after a door.

### 5.4 The frame-rate limit moves

`[Video] framerate limit` becomes the driver's `minimumIntervalUs` on the paced path. The
driver's limiter is the sleep, so it lands before input where the host's landed after the
present: a player with a limit set gets lower latency from the move alone. The loading screens
keep their own limiters through `Environment::getFrameRateLimit`, and those double the
driver's for the length of a load, which is harmless.

### 5.5 Boost on a laptop

`lowLatencyBoost` holds the top clock. On this machine that is a 4090 Laptop on a MUX; the
thermal answer is the user's, and `boost` is a named choice and not a default.

### 5.6 The rasterizer

`awaitFrame` moves the limiter's call from the loop's end to its start. The measured frame
duration is the same interval seen from the other side; the phase 1 test pins that the answer
is what `limit()` answered.

---

## 6. Sources

- [VK_NV_low_latency2 — Vulkan Documentation Project](https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_low_latency2.html)
- [vkLatencySleepNV](https://docs.vulkan.org/refpages/latest/refpages/source/vkLatencySleepNV.html), [VkLatencySleepInfoNV](https://docs.vulkan.org/refpages/latest/refpages/source/VkLatencySleepInfoNV.html), [vkSetLatencySleepModeNV](https://docs.vulkan.org/refpages/latest/refpages/source/vkSetLatencySleepModeNV.html), [VkLatencySleepModeInfoNV](https://docs.vulkan.org/refpages/latest/refpages/source/VkLatencySleepModeInfoNV.html), [VkLatencyMarkerNV](https://docs.vulkan.org/refpages/latest/refpages/source/VkLatencyMarkerNV.html), [VkLatencyTimingsFrameReportNV](https://docs.vulkan.org/refpages/latest/refpages/source/VkLatencyTimingsFrameReportNV.html), [VkSwapchainLatencyCreateInfoNV](https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainLatencyCreateInfoNV.html)
- The extension's definitions as installed: `/usr/include/vulkan/vulkan_core.h`, `VK_NV_LOW_LATENCY_2_SPEC_VERSION 2`
- [NVIDIA nvapi.h](https://github.com/NVIDIA/nvapi/blob/main/nvapi.h): `NvAPI_D3D_SetSleepMode`, `NvAPI_D3D_Sleep`, `NvAPI_D3D_SetLatencyMarker`, `NvAPI_D3D_GetLatency` — the D3D form of the same contract, with the integration notes quoted in §1
- [Streamline Reflex programming guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideReflex.md) and [PCL guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuidePCL.md)
- DXVK: [`dxvk_presenter.cpp`](https://github.com/doitsujin/dxvk/blob/master/src/dxvk/dxvk_presenter.cpp), [`dxvk_cmdlist.cpp`](https://github.com/doitsujin/dxvk/blob/master/src/dxvk/dxvk_cmdlist.cpp), [`dxvk_latency_reflex.cpp`](https://github.com/doitsujin/dxvk/blob/master/src/dxvk/dxvk_latency_reflex.cpp)
- [Phoronix on Vulkan 1.3.266 and the extension](https://www.phoronix.com/news/Vulkan-1.3.266-Released)
