# A plan for the open issues

Ordered by what this fork ranks: how it looks first, then performance, then what keeps the gates
honest. Each item states the change, the files it lands in, and how to tell that it worked.

`.notes/ISSUES.md` holds the findings. This file holds only the work.

## 1. The gates that cannot report a failure

**The suite reports success after it skips every GPU regression.** Nothing below can be trusted
until this is fixed, because every check in this plan runs through the same harness.

One exception type answers two different questions today. `Rtx::Error` means both "this machine
cannot run the backend" and "this code broke a contract", and `createVulkanRenderer` hands the
harness a string either way.

- Add `Rtx::Unsupported : Error` to `components/rtx/error.hpp`. It means bring-up only: no loader,
  no driver, a device that does not qualify, a missing extension, a format the device refuses.
- Change the throws in `PhysicalDevice::select`, the instance and the extension checks to the new
  type. Leave `image.cpp:57` and every other contract as `Error`.
- `createVulkanRenderer` at `vulkanrenderer.cpp:1468` catches `Unsupported` and returns a reason.
  It lets `Error` leave, so gtest reports it as a failed test.
- `findInstanceObstacle` at `harness.cpp:144` narrows its catch the same way.

Verify: `components-tests --gtest_filter='Rtx*'` still passes 567 tests. Then add an unlisted
`VkFormat` to one image temporarily, and confirm the suite **fails** rather than skips.

## 2. Three defects in the picture

### 2.1 The moons lose their light at the shadow threshold

`FOG_SHAFT_FLOOR` decides whether a moon is worth a shadow ray. `fogscatter.comp:232` reads the same
flag to zero the moons' energy, so the light steps to nothing at the threshold instead of losing
only its shadow.

- Split the two decisions. Keep `FogSources::mMoonlit` as the ray gate. Compute `moons` from
  `moonsInAir` whenever `HAS_MOONS` holds and the irradiance is nonzero, with `lunar` at one where
  no ray was cast.
- `HAS_MOONS` still folds the whole block away on a moonless frame, which is what that constant is
  for.

Verify: `shot --views=balmora-fog-night`, and a second shot with `FOG_SHAFT_FLOOR` raised far enough
to gate every moon. The two must agree to within the shadow the ray would have cut. The cost is two
`exp` per froxel, so read the `air` zone before and after.

### 2.2 Particles and cloud shells are attenuated by a midpoint sample

`sprites.glsl:384` and `medium.glsl:166` take one `fogExtinctionAt` halfway along the view path and
raise it to the whole span. Opaque geometry at that distance goes through the volume, so the two
disagree over an even height layer, which is the one case with an exact answer.

- Call `fogColumn` for the even case, which is the closed form already written at `fog.glsl:452` and
  reached by nothing. Both sites become `exp(-fogColumn(origin, direction, span))`.
- Where the coverage field is on — `frame.mFogUniform < 1.0` — the closed form is not exact either.
  Keep the midpoint sample there, guarded by the same `FOG_UNIFORM` variant constant that
  `fogExtinctionAt` uses, so an even frame compiles the field read out entirely.
- Fix the comment in `fogAlong` at `fog.glsl:698`. It names `fogColumn` as a live reader today.

Verify: `shot --views=balmora-fog-night` and `--views=seyda-neen-ship-overcast`, where a puff stands
in front of hazed geometry. The puff and the wall behind it must fade together. Extend the fog test
in `apps/components_tests/rtx/visibility/fog.cpp` with a puff and a wall at one distance, and assert
that both carry the same transmittance.

### 2.3 A pane takes the haze of the surface behind it

`visibility.rgen:166` composites the pane before the water and the air, so the pane is charged for
the whole path rather than for its own. The code states this as an accepted over-darkening.

- Keep the pane's own distance where the peel is made, near `visibility.rgen:127`, before the second
  trace overwrites `distance`.
- Move the composite after the water and the air. Attenuate `paneRadiance` over the pane's distance
  and what is behind it over the full one, then combine.
- This costs a second `fogAlong` and a second `waterColumn` on a pane pixel only.

Verify: `shot --views=balmora-mages-guild` and `--views=vivec-canalworks`, looking through glass at
a hazed interior. Read the `trace` zone to price the second column.

## 3. Faded actors show one layer

`visibility.rgen:123` peels the nearest translucent surface only, so a person under Chameleon shows
a cuirass faded and the skirt under it solid. The comment names an ordered walk as the answer.

This is a feature rather than a repair, and it is the widest item here. Take it after everything
above.

- Walk translucent hits in order to a fixed budget, compositing front to back, with the budget stated
  as a constant in `look.h`. `crossingsAlong` under `COUNT_CROSSINGS` already measures the census
  that would size it: Red Mountain reaches eight.
- A budget of one is the behaviour today, which makes the change measurable against itself.

Verify: `shot` of a fading actor at several budgets. Price each on `--views=balmora`, where actors
stand, and read the `trace` zone.

## 4. Cell crossings hold the tail

The largest performance item in the tree. `bench --views=island-crossing --seconds=10` renders a
median frame of 6.7-7.2 ms and a worst of 178-202 ms. 19 crossings take 1.9 s of an 8.6 s run.

The work is on the host and not the device: `walk` reaches 53-89 ms and `place` 27-30 ms, where the
whole device frame is near 12 ms. 1.2-1.3 s of the 1.9 s is reading, and 0.6 s is building. None of
the crossings is a rebuild, so this is arrival cost and not structure cost.

Uniform frame time is the standard here, so the answer is not a faster crossing. It is a crossing
that never lands on one frame.

- Profile first, and do not guess. `apps/rtxtool/profile.sh` records the host over the measured
  frames. The split between reading and building is already printed, so the question a profile
  answers is which calls inside the 1.2-1.3 s of reading hold the time.
- Move the read off the frame path. A ring arrives on a worker, and the frame adopts what is ready.
- Adopt incrementally. A frame takes a bounded number of instances, so a crossing spreads over the
  frames it needs rather than blocking one.
- Keep the budget stated as a constant, and measure the worst frame against it.

Verify: the same three interleaved runs on a hot card. The target is the p99 and the worst frame,
with the median no worse. `--seconds=10` on one view costs thirteen seconds, so six alternations
come in under two minutes. Report the clock and the temperature beside each run.

## 5. Tests over one second

Two of the three build a renderer of their own, because the upscale mode is fixed when a renderer is
built. The harness already holds `Once<T>` for exactly this.

- Key the cached renderer on the upscale mode as well as on validation, in `harness.cpp:83`. Both
  `RtxUpscaledFrameTest` and `RtxUpscalerStabilityTest` then share one upscaling renderer, and the
  suite pays 1.6 s once rather than twice.
- `RtxVisibilityTest.theVolumeLightsTheAirUpToASurfaceWhereverInASliceItStands` is 48 traced frames
  and needs a different answer. The volume settles after `settled` frames, so measure what it
  actually needs and cut the count to it.

Verify: `components-tests --gtest_filter='Rtx*'`, and read the per-test times. No test may pass one
second, and the run must stay under 30 s.

## 6. Two reports that mislead

### 6.1 The bench zone row

`describeZones` at `components/rtx/frametimes.cpp:115` prints each zone's median over the frames
that ran it. A reader sums the row, or sets one zone against the frame median. Neither is valid:
`micromap 7.51` sits above a frame median of 6.73 ms.

- Print the share of frames each zone ran in, wherever it is not all of them. A zone that ran in
  every frame stays as it reads today.

Verify: `bench --views=island-crossing --seconds=10` names micromap as a pass that runs at a
crossing. `bench --views=balmora` leaves every zone unmarked.

### 6.2 The harness warning

`runInfo` at `apps/rtxtool/main.cpp:336` skips `mCacheDirectory`. Its caller at line 554 holds
`command.mConfig`, which is where `runShot` takes the same path from.

- Give `runInfo` the cache directory as a parameter and name it in the initializer.
- `info` compiles a renderer, so a warm cache also makes the command faster.

Verify: `ninja openmw-rtxtool` warns nothing, and `openmw-rtxtool info` still reports the device.

## 7. The two disagreements the fog tests log

`apps/components_tests/rtx/visibility/fog.cpp` widens two tolerances to 14 levels and to 0.12, and
both point here. The closed form that the volume replaced agreed exactly on the first and to within
0.006 on the second, so the volume is what changed.

These are two separate defects and want separate work. Take them after items 1 to 3, and treat each
as its own investigation.

- **The dry-cell fallback.** A dry cell is handed minus infinity and falls back to sea level, which
  is where a water level of nought puts the layer. `fogBase` and `fogPools` are where the two paths
  part. Read what each hands the volume for the same ray, at the same slice.
- **The sun a fifth over.** The volume reads 0.597 where the closed form reads 0.500. A fifth is not
  a factor of `4 pi`, so this is not the convention. The suspects in order: the slice a level ray
  lands in, `fogBeamDepth` against the closed form's `column`, and the phase the volume divides out
  against the one the trace puts back.

Verify: tighten each tolerance to what the closed form held, and let the test say when it is right.
