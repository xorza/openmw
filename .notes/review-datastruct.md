Delete an item when it is addressed. This file lists open findings only.

# Data-structure review: the whole RTX suite

Reviewed 2026-09-06 against a clean tree on `74d6702ce1`.

Scope: `components/rtx/`, `components/rtxvulkan/`, `components/rtxbackends/`,
`components/rtxbench/`, `components/surface/`, `components/myguirtx/`,
`apps/openmw/mwrender/rtx/` and `apps/rtxtool/`.

Static review of types, ownership and call graphs. Test structure and the API that tests use are out
of scope. No measurements were taken.

---

## `MWRender::Session` drives the run and writes everything the run produces

`session.hpp` is 502 lines and `session.cpp` is 1082. One class holds the schedule, the camera, the
weather turn, the checks, the report, the record, the hashes and seven writers.

- [ ] **Split the writers off the session.** `writeSheet`, `writeMapTile`, `writeDoll`,
      `reportFound`, `runChecks`, `writeView` and `finish` are what `Actions` asks for. They need a
      renderer and a path, and they need nothing of the schedule. The session should hand an
      `Actions` and a renderer to one object and stop.

- [ ] **Split the report off the session.** `mPlaces`, `mHeader`, `mReport`, `mExitStatus`,
      `mChecked`, `mFailed` and `Held` are one record under construction. `components/rtxbench`
      already owns `BenchPlace` and `BenchHeader`.

- [ ] **`installSession` and `publishSessionResult` are a global mailbox.** `session.cpp` keeps
      `sInstalled` and `sResult` at file scope, and four free functions read and write them. The
      header explains why `RendererSpec` cannot carry the request, and that reason covers the
      request only. `SessionResult` travels back the same way for no stated reason.

- [ ] **`FrameRequest` reaches the renderer through the settings registry.**
      `main.cpp:391` writes eleven fields into `Settings::rtx()` and `Settings::video()`, and
      `RtxRenderer` reads five of them back into `mDelight`, `mShowAlbedo`, `mFilter`, `mJitter` and
      `mExposure`. The registry is a string-keyed global between two objects in one process.

---

## Ownership placed for convenience rather than for what needs it

- [ ] **Every `OffscreenTrace` owns a terrain baker.** `OffscreenTrace` owns a `SceneUploader`, and
      `SceneUploader` owns a `CompositeQueue`. That queue holds a `std::jthread`, a `ShadingCache`,
      three scratch vectors and a `CompositeScratch`. The queue's own comment says a doll never
      asks. Move the queue to whatever owns the world, and hand it to `hand()` as the argument that
      `describeAll` and `describe` already take.

- [ ] **The frame's target chain and the interface picture's chain are written twice.** The frame
      has `mColour`, `mTarget`, `mSpare`, `mPresented`, `mSum`, `mChannels` and `mFogVolume`. The
      picture has `mViewChannels`, `mViewFogVolume`, `mViewColour`, `mViewTarget`, `mViewWidth` and
      `mViewHeight`. `createTargets` and `growViewTargets` are two spellings of one operation, and
      `mAccumulate`/`mFilter` are doubled as `mViewAccumulate`/`mViewFilter`. Name the chain once.

- [ ] **`components/rtx/frametimes.hpp` holds the report and one clock helper.** `FrameTimes`,
      `FrameSamples`, `GpuZone`, `GpuBreakdown` and the five `describe*` functions are read only by
      `components/rtxbench` and by the session. `framering.cpp` and `worldmirror.cpp` include the
      file for `since()` alone, and the file includes `renderer.hpp` for `GpuSpan`. Move the report
      to `components/rtxbench` and put `since()` where a backend can reach it cheaply.

- [ ] **`SceneTextures` keeps three arrays that share one index.** `mImages`, `mKept` and `mLightOf`
      are parallel, and the header says so. One vector of a per-slot struct removes the rule.

- [ ] **`TextureTable` asks two strings whether a slot is free.** A slot is a file or a bake and
      never both, and `isFree` tests `mPaths[t].empty() && mBaked[t].empty()`. A kind field beside
      one name says the same thing once. The two spans the backend reads can still be handed out.

---

## Four implementations of "a list of slots, each named once"

- [ ] **Make one type and use it four times.** `Rtx::SlotSet` is a list, a flag byte and a stale
      bit. `Rtx::SlotChanges` is two lists and a news byte. `Rtx::RowDebt` is a list, a
      `vector<bool>` and an everything flag. Each header argues, correctly and at length, that the
      list and the flag must live together. Three headers making one argument is one type missing.

- [ ] **`SlotBlocks::mOwed` has no flag at all.** `SlotBlocks::write` at `slottable.hpp:232` pushes a run onto every copy's
      list without asking whether it is already there. A run named twice in one frame is copied
      twice. This is the exact failure the three types above exist to prevent.

---

## `Span` exists and no table stores one

`SpanAllocator::allocate` returns a `Span`. Every caller takes it apart at once and stores the two
halves as separate fields. Every release builds a `Span` again from those fields.

- [ ] **`MeshRange` restates a span four times.** It holds `mVertexOffset`, `mVertexCount`,
      `mIndexOffset` and `mIndexCount`. Two `Span` members say it.

- [ ] **`Material` stores `mLayerOffset` and `mLayerCount`.** `materialresolver.cpp:222` receives a
      `Span` from `addLayers` and splits it on the next two lines. `scenedesc.cpp:692` puts it back
      together to release it.

- [ ] **`Rig`, `Morph`, `MaterialLayer` and `SpriteEmitter` do the same.** `Rig` holds
      `mRunOffset`, `mInfluenceOffset` and `mInfluenceCount`, and its run length is implied by
      `mVertexCount`. `MaterialLayer` holds `mMaskOffset` with the grid sides. `SpriteEmitter` holds
      `mFirst` and `mCount`.

---

## Headers reach for the whole scene description to get one typedef

`index.hpp` exists so that a table can name an `Index` without including the file that includes it.
Several headers do not use it.

- [ ] **`components/rtxvulkan/frameslots.hpp` includes `scenedesc.hpp` for `Index` alone.** So does
      `components/rtxvulkan/slottable.hpp`. Both are template headers included widely in the
      backend, and both pull 1151 lines, OSG, `shaders/scene.h` and `shaders/skinning.h`.

- [ ] **`moonbuilder.hpp`, `nightsky.hpp`, `skybuilder.hpp` and `texturebuilder.hpp` need only
      `Index`, `sNoIndex` and a forward declaration of `SceneDesc`.** None of them names a member of
      the class.

- [ ] **`lightbuilder.hpp` needs only `Light`.** `Sun` has its own header now, so what is left is
      the one type `SceneDesc` really holds.

---

## `lightbuilder.hpp` is four subjects in one header

417 lines declare the light of a `LIGH` record, the sun, the sky budget, the air over a moon, the
weather's name table, and the colour decode.

- [ ] **Split it by subject.** A `LIGH` becoming a light, a weather becoming a sky, and a colour
      being decoded are three questions. `decodeColour` alone is named by nearly every file that
      includes this header, and it drags `esm3/loadcell.hpp`, `sceneutil/lightcontroller.hpp` and
      `sky/timeofday.hpp` in with it.

---

## Documentation blocks that describe the wrong member

Each of these reads as the member's contract and belongs to a different one. A reader who trusts them
is misled about ownership.

- [ ] **`sceneacceleration.hpp:304`.** The block describes the blocked position buffer — host
      writes, `SkinPass`, the refit's copy, the bind pose. The member under it is
      `Owned<VkQueryPool, vkDestroyQueryPool> mCompactable`. The real `mPositions` at line 340 has no
      comment. Two doc blocks are also merged: the compaction pool's own text starts mid-block at
      "One slot per mesh".

- [ ] **`texture.hpp:99`.** The `TextureArray` class doc runs into the `SetApart` doc, so `SetApart`
      keeps the last sentence and `class TextureArray` has none.

- [ ] **`instancerecord.hpp`.** `updateInstanceRecords` carries two summary sentences from two
      merged blocks. `InstanceRecord::mMask` has a blank comment line in the middle of a sentence.
