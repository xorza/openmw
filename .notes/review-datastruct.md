Delete an item when it is addressed. This file lists open findings only.

# Data-structure review: the whole RTX suite

Reviewed 2026-09-06 against a clean tree on `74d6702ce1`.

Scope: `components/rtx/`, `components/rtxvulkan/`, `components/rtxbackends/`,
`components/rtxbench/`, `components/surface/`, `components/myguirtx/`,
`apps/openmw/mwrender/rtx/` and `apps/rtxtool/`.

Static review of types, ownership and call graphs. Test structure and the API that tests use are out
of scope. No measurements were taken.

---

## One fact, two types, and a conversion written by hand

Each pair below states the same thing twice. The tree already counts one of these drifts with a
canary instead of removing the second copy. Every item here is a place where a field added to one
half reaches the other only if somebody remembers.

- [ ] **`RtxTool::Place` restates `Rtx::Stand` plus `StopSky`.** `Place` resolves the hour and the
      weather into plain values, and `stopFrom` puts them straight back into `StopSky`'s optionals.
      Both types are reachable from one namespace now, so `chooseView` could hand back the `Stop`
      it is about to become. What `Place` buys is a type shape that says the conditions are
      settled, and only `stopFrom` reads them.

- [ ] **`Surface::AlphaMode` and `Rtx::AlphaMode` hold the same three enumerators.**
      `materialresolver.cpp:392` converts one to the other with a switch. The core can name the
      content's enum, as it already names `Surface::Material`.

- [ ] **`Surface::Material::mTextureScale` and `mTextureOffset` become
      `Rtx::Material::mTextureTransform`.** One transform, two spellings, and a conversion at
      `materialresolver.cpp:414`. Both sides could hold the `uv * xy + zw` form the terrain layers
      already use.

- [ ] **The material is on the mesh and on the placement.** `MeshRange::mMaterial` is read by
      `SceneMicromaps`. `MeshInstance::mMaterial` is read by the shading table.
      `ExtractionStats::mWornOtherwise` counts the frames on which the two disagree. A canary that
      watches a duplicate is evidence that the duplicate should go.

---

## `SceneDesc` holds four tables that share nothing but the class

`scenedesc.hpp` is 1151 lines. The class carries about forty members. Two of its tables already have
types of their own — `PlacementTable` and `TextureTable` — and each says in its own header why a
table, its free list and its change lists are one invariant. The rest of the tables have not been
given the same treatment, so their free lists, keep sets, span allocators and arrival lists sit
loose in one class beside each other.

- [ ] **Give the mesh table its own type.** It is `mPositions`, `mNormals`, `mTexCoords`, `mIndices`,
      `mMeshes`, `mFreeMeshes`, `mKeptMeshes`, `mMeshChanges`, `mVertexRuns`, `mIndexRuns` and
      `mMeshRevision`. Eleven members and one invariant.

- [ ] **Give the deformer tables their own type.** It is `mRigs`, `mRuns`, `mInfluences`, `mMorphs`,
      `mMorphOffsets`, `mBones`, `mWeights`, `mFreeRigs`, `mFreeMorphs`, `mArrivedRigs`,
      `mArrivedMorphs`, `mDeformed`, `mBindRuns`, `mBoneRuns`, `mWeightRuns`, `mRigRuns`,
      `mInfluenceRuns` and `mMorphRuns`. Eighteen members, and `releaseDeformer` and `notePosed` are
      already its private methods.

- [ ] **Give the material table its own type.** It is `mMaterials`, `mLayers`, `mMasks`,
      `mFreeMaterials`, `mKeptMaterials`, `mWrittenMaterials`, `mArrivedLayers`, `mArrivedMasks`,
      `mLayerRuns` and `mMaskRuns`. `holdMaterialTextures`, `dropMaterialTextures`,
      `forEachMaterialTexture` and `noteMaterial` go with it.

---

## A second host that no longer exists

`openmw-rtxtool` drives a real game now. The harness no longer reads cells, derives a sky from the
content files, or builds a scene of its own. Many headers still promise "one call and two callers",
and the second caller is gone. What is left is production code that only the tests reach.

- [ ] **`makeDaylight`, its three overloads, and the whole weather-record path have no production
      caller.** `WeatherRamps`, `readWeatherRamps`, `requireWeather` and `nextRegionWeather` are
      reached only from `lightbuilder.cpp` itself and from `apps/components_tests/rtx/`. The game
      builds its `Daylight` field by field in `readworld.cpp` instead.

- [ ] **`Rtx::makeMoon` and `Rtx::moonAngularRadius` have no caller.** The game calls `placeMoon`
      with the angles the weather system settled on. `moonAngularRadius` is a one-line forwarder to
      the private `angularRadiusOf`.

- [ ] **`Rtx::stormDirection` has no caller and forwards to `Weather::stormDirection`.**
      `lightbuilder.cpp:474` only turns a weather index into a name first.

- [ ] **`Rtx::sunAbove` and `Rtx::castsWherePlaced` are exported and used only inside their own
      `.cpp`.** Make them private, or drop them.

- [ ] **`apps/rtxtool/main.cpp` includes five headers it does not use.** They are `fogbuilder.hpp`,
      `lightbuilder.hpp`, `texturebuilder.hpp`, `scenedesc.hpp` and `sceneextractor.hpp`. The file
      names no symbol from any of them. This drags OSG and the whole scene description into the
      command-line parser.

- [ ] **Correct the headers that name two hosts.** `frameworld.hpp:87`, `lightbuilder.hpp:146`,
      `moonbuilder.hpp`, `skybuilder.hpp` and `fogbuilder.hpp` each say the game and the harness
      reach a number by different routes. Only the game does. The prose now argues for a split that
      the tree no longer has.

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

## `lightbuilder.hpp` is five subjects in one header

528 lines declare the light of a `LIGH` record, the sun, the sky budget, the weather record, the
region's weather table, the air over a moon, and the colour decode.

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
