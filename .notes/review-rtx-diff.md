# Review: the whole diff against upstream/master

Scope: every production file the fork adds or edits against `upstream/master`
(merge base `97c3f81aba`) — `components/rtx*`, `components/surface`,
`components/myguirtx`, `apps/rtxtool`, `apps/openmw/mwrender/rtx`, the lifted
`components/{sky,weather,terrain,bodyparts}`, and the upstream files the seam
touches. Test files are out of scope.

**Delete an item when you have addressed it.** This file lists open findings only.

---

## The checks gate cannot pass, for two reasons that are the suite's and not the renderer's

`CI/check_rtx_checks.sh` is this fork's own gate. Run at Addamasartus it reports
six checks ok and two FAIL, and the two fail the same way with the renderer
reverted four commits, so neither is a regression.

- [ ] `apps/rtxtool/main.cpp:762-808` — `commandCheck` leaves the upscaler at the
  default `quality`, and `Check::PictureSettles` asks whether two frames of a held
  camera hash the same. Ray Reconstruction is temporal, so they never do and the
  check can never pass. `commandVerify` already states exactly this and forces
  `--upscale=off` for it (`main.cpp:610-614`): "Ray Reconstruction is temporal and
  carries state nothing below can hold still". Either `check` sets the same, or
  `PictureSettles` joins `CrossingsAppend` as a check asked only where it can be
  answered — that loop is already there, at `main.cpp:790-792`.
- [ ] `apps/openmw/mwrender/rtx/checks.cpp:106-108` — `Check::SurfacesDescribed`
  reports "1 placements wore a material nothing described" at **every** place of
  the default suite, indoors and out, so the gate is red everywhere whatever the
  upscaler does. Exactly one, everywhere, points at something every world has
  rather than at a cell's content. `ExtractionStats::mUndescribedMaterials` is
  what the check reads and `MaterialResolver::readMaterial` is what counts it —
  find which drawable reaches it with no `Surface::Material` on its state set
  before deciding whether the check or the extractor is wrong.

## State that is written and never read

- [ ] `apps/openmw/mwrender/rtx/session.hpp:461-462` — `mChecked` and `mFailed`
  are incremented in `runChecks` (`session.cpp:1025-1029`) and read by nothing.
  Either report them in `finish()` beside the check lines, or remove both.
- [ ] `apps/openmw/mwrender/rtx/rtxrenderer.cpp:762-764` — `mSpentMs` is
  accumulated only when `finishFrame` returned a result, but `mTimed` counts
  every frame. The periodic line divides by frames that contributed nothing, so
  "waited N ms a frame" is an underestimate by an unknown factor.

## Public members with no caller anywhere

Each of these is declared, defined, and reached by no production code and no
test. Remove them.

- [ ] `components/rtx/compositequeue.hpp:99` — `getWaitingCount`.
- [ ] `components/rtxvulkan/blockedbuffer.hpp:55` — `getBlockSize`.
- [ ] `components/rtxvulkan/dlss.hpp:104` — `getCapabilities`.
- [ ] `components/rtxvulkan/dlsspass.hpp:119-120` — `getRenderExtent` and
  `getOutputExtent`.
- [ ] `components/rtxvulkan/pipelinelayout.hpp:41` — `getSetLayout`.
- [ ] `components/rtxvulkan/gputimer.hpp:45` — `isSupported`.
- [ ] `apps/rtxtool/viewpoint.cpp:92` / `viewpoint.hpp` — `describeProfile` has
  no production caller; only the test suite reaches it. It is also incomplete
  for what its comment claims: the line it builds omits `--delight`,
  `--distant-cells`, `--distant-statics`, `--jitter` and `--crossings`, and
  the first two change the picture. Either wire it to a verb or delete it.

## An enum's spellings are restated in prose, and the copies have drifted

Four enums each carry a hand-written `xName` switch and a hand-written `xNamed`
if-chain over the same strings, and the list of strings is then restated in
option help, in runtime error messages, in `rtx.hpp` and in
`settings-default.cfg`. Two of those copies are already wrong.

- [ ] `apps/rtxtool/options.cpp:157` and
  `apps/openmw/mwrender/rtx/rtxrenderer.cpp:139` — both list "off, performance,
  balanced, quality or dlaa" and omit `ultraperformance`, which
  `Rtx::upscaleNamed` accepts and `files/settings-default.cfg:1286` documents.
  A user who types the mode gets an error naming the modes without it.
- [ ] `apps/openmw/mwrender/rtx/rtxrenderer.cpp:209` and
  `components/settings/categories/rtx.hpp` (`mReorder`, line 100) — both say "off, hit or
  hint" and omit `both`, which `Rtx::reorderNamed` accepts and
  `apps/rtxtool/options.cpp:167` documents.
- [ ] `components/rtx/upscale.hpp:48-129`, `components/rtx/reorder.hpp:38-68`,
  `components/rtx/reconstruction.hpp:30-95` — replace each `xName`/`xNamed`
  pair with one `constexpr std::array<std::pair<Enum, std::string_view>, N>`
  and derive both directions and the printable list from it. `apps/rtxtool/verbs.cpp:17`
  and `apps/openmw/mwrender/rtx/checks.cpp:37` already do exactly this, and say
  in their comments why: "The one list of the names."

## One knob, three defaults

- [ ] `apps/rtxtool/options.cpp:280` defaults `--distant-cells` to `5.0f`;
  `apps/rtxtool/framerequest.hpp:79` defaults `FrameRequest::mDistantCells` to
  `4.0f`; `files/settings-default.cfg:1284` says `distant land cells = 4`. The
  harness therefore builds a world a cell wider than the game does by default,
  which is the exact class of drift `applyHostedSettings`'s own comment says the
  settings channel exists to remove.
- [ ] `apps/rtxtool/options.cpp:280` — the help text for `--distant-cells` opens
  "with `--distant-terrain`", which is not an option. The one that exists is
  `--distant-statics`.

## The option-ownership table covers one frame knob and not the rest

`ToolOptions::complainAbout` exists so that "a run that names it under any other
command is stopped rather than quietly rendering something else". Only `--fov`
is registered under `sFramed`.

- [ ] `apps/rtxtool/options.cpp` — `--size`, `--upscale`, `--preset`,
  `--reorder`, `--delight`, `--filter`, `--albedo`, `--jitter`, `--exposure`,
  `--crossings`, `--hour`, `--weather`, `--day`, `--distant-statics` and
  `--distant-cells` all use the unowned `addOption`, so `readsOption` answers
  `Verbs::Every`. `openmw-rtxtool info --size=800x600 --delight=0` is accepted
  and ignored. Give them `sFramed` (or `sFramed | Verbs::Doll` where `doll`
  builds a `FrameRequest`), which is what `--fov` already has.

## Comment blocks lost their paragraph breaks, so a paragraph documents the wrong line

Two kinds, one cause: a `///` block that should be two paragraphs, or two
declarations' documentation, got joined.

A doc comment now sits on a declaration it is not about:

- [ ] `components/rtx/renderer.hpp:435-436` — "What one traced frame came to."
  is `FrameResult`'s summary and sits on `struct GpuSpan`. `FrameResult`
  (line 450) has none.
- [ ] `components/rtx/scenedesc.hpp:438-441` — "One mesh placed in the world: a
  row of the top-level acceleration structure." is `MeshInstance`'s summary and
  sits on `struct Sprite`.
- [ ] `components/rtx/frameworld.hpp:26-51` — the six-paragraph summary of
  `struct FrameWorld` (including the three-bugs list) sits on `rainOnWater`.
  `struct FrameWorld` at line 98 has no comment and a stray blank line after
  its brace.
- [ ] `apps/openmw/mwrender/rtx/rtxrenderer.hpp:51-68` — the summary of
  `RtxRenderer` sits on the forward declaration `class TracedView;`.
- [ ] `apps/openmw/mwrender/rtx/rtxrenderer.hpp:331-337` — `mMirror`'s comment
  is two comments spliced; the first sentence ends mid-clause at "a subtree the
  world and a doll can both reach must".
- [ ] `components/rtx/scenedesc.hpp:1041-1046` — a stale `//` block describing
  "Two flat arrays rather than one struct" sits above `PlacementTable
  mPlacements`, which is one struct, immediately above the `///` comment that
  says so.
- [ ] `components/rtx/sceneextractor.cpp:80-81` — `/// The texture bound at
  `unit`, or null.` is the last line of an anonymous namespace and documents
  nothing.
- [ ] `apps/openmw/mwrender/rtx/worldmirror.cpp:109-112` — a copy of the "Where
  the benchmark's `walk ms` starts" comment from
  `rtxrenderer.cpp:599-601`, attached to no statement. The timing really starts
  in `rtxrenderer.cpp:602`.
- [ ] `components/rtx/lightbuilder.hpp:360-362` — a bare `///` line between the
  paragraph and the `@param` block of `requireWeather`.

A paragraph break inside one block was dropped, so the second paragraph reads
as a continuation of the first (`components/rtx/lightbuilder.hpp` style is a
blank `///` between paragraphs):

- [ ] `components/rtx/sceneextractor.cpp:581`
- [ ] `components/rtx/texturebuilder.cpp:281`
- [ ] `components/rtx/renderer.hpp:454`
- [ ] `components/rtxvulkan/presenter.cpp:131`
- [ ] `apps/openmw/mwrender/rtx/readworld.cpp:25` and `:34`
- [ ] `apps/openmw/mwrender/rtx/rtxrenderer.cpp:717` and `:736`

## Six harness commands repeat the same four-line preamble

- [ ] `apps/rtxtool/main.cpp:524-596, 689-715, 726-753` — `commandTextures`,
  `commandDoll`, `commandMap`, `commandScene`, `commandShot` and `commandView`
  each open with `chooseView` / `frameFrom(*place.mHour, *place.mWeather)` /
  `applyHostedSettings` / `stillStopAt`, then set one or two `Actions` fields.
  One helper returning the `{place, frame, stop}` triple leaves each command as
  the two lines that differ. It also states the unchecked `*place.mHour`
  dereference once instead of six times.
- [ ] `apps/rtxtool/main.cpp:628-630` and `:781-786` — `commandVerify` and
  `commandCheck` open-code `stillStopAt`'s three assignments instead of calling
  it, so a change to what "held still" means has three places to reach.
- [ ] `apps/rtxtool/main.cpp:601-606, 647-652, 765-770` — `commandVerify`,
  `commandBench` and `commandCheck` read the hour and the weather twice by two
  different rules in one command: raw `variables["hour"]` for `frameFrom`, then
  `applyConditions` which uses `hourGiven`/`weatherGiven`. A view that fixes an
  hour therefore stands under one hour and is *framed* for another.

## Duplicated blocks inside one function

- [ ] `apps/openmw/mwrender/rtx/session.cpp:363-367` and `:376-379` —
  `beginStop`'s free-camera branch and its final `else` branch compute `mFrom`
  and `mFromLook` with identical code. Hoist it above the `if`.
- [ ] `apps/openmw/mwrender/rtx/session.cpp:346` and `:363` — `const
  MWWorld::Ptr player` is declared twice, the second shadowing the first inside
  the free-camera branch. The comment beside the inner one explains where a
  position lives, not why a second `getPlayerPtr()` is needed.
- [ ] `components/rtx/scenedesc.cpp:427-447` — `holdMaterialTextures` and
  `dropMaterialTextures` are the same four-step walk with one call swapped. One
  `forEachMaterialTexture(material, fn)` removes the pair and the risk that a
  role added to one is forgotten in the other.

## A per-frame `dynamic_cast` chain the cache beside it already covers

- [ ] `components/rtx/materialresolver.cpp:105-149` — `animate` is called for
  every node of the graph every frame. For every node that has any callback it
  runs `findUpdater`, which walks both callback chains and `dynamic_cast`s each
  link, *before* looking the node up in `mAnimated`. The entry is already keyed
  on the node and already lives across frames — store the `StateSetUpdater*` in
  it and the chain walk happens once per node instead of once per node per
  frame. Every other cast on this walk is gated on `isFrom(node, ...)` for
  exactly this reason (`sceneextractor.cpp:257`, `:378`, `:662`).

## Two contracts for one keep set, and they disagree

- [ ] `components/rtx/scenedesc.cpp:610-624` — `release`'s early return compares
  `meshes.size()` against the live count, which is only sound if the keep set
  holds no duplicates. `markKept` (`:594-596`) documents the opposite:
  "Duplicates and any order are fine." Today the callers happen to be
  duplicate-free, so the code works and one of the two comments is a trap. Say
  it once — assert the set is unique, or count uniques.

## Two parallel families of build scratch in `SceneAcceleration`

- [ ] `components/rtxvulkan/sceneacceleration.hpp:313-365` — `mBuildGeometries`,
  `mBuildMicromaps`, `mBuilds`, `mBuildRanges`, `mBuildSizes`,
  `mBuildScratchOffsets`, `mLiveBuilds`, `mBuildRangePointers` and the parallel
  `mRefitGeometries`, `mRefitMicromaps`, `mRefitBuilds`, `mRefitRanges`,
  `mRefitRangePointers` are the same shape twice, filled by `buildMeshes` and
  `prepareRefit` with the same sequencing rule ("the geometries are sized before
  any build info names one"). One `StructureBuildBatch` holding the vectors and
  that rule, instantiated twice, removes thirteen members and one of the two
  places the rule can be got wrong.
- [ ] `components/rtxvulkan/sceneacceleration.hpp:76-77` — the deleted copy
  constructor and assignment sit after `build()`, away from the constructor and
  destructor at lines 58-59.

## `SceneDesc::clear()` names forty-four members by hand

- [ ] `components/rtx/scenedesc.cpp:718-764` — a member added to `SceneDesc` and
  forgotten here is a scene that keeps a departed world's table with no
  diagnostic. `mKeptMeshes` and `mKeptMaterials` are already absent from the
  list; they happen to be safe because `markKept` clears them. Group the
  members into sub-objects that clear themselves, the way `PlacementTable` and
  `TextureTable` already do, so the list shrinks to the things that are really
  `SceneDesc`'s own.
- [ ] `components/rtx/scenedesc.cpp:718` — `SceneDesc::clear()` has no production
  caller at all, and `components/rtx/renderer.hpp:554` states as much:
  "`SceneDesc::clear` is never called on it". So `getResetRevision()` never
  moves, `SceneUploader`'s `Kind::Rebuilt` path is reached only by an uploader
  that has never seen the pair in front of it, and `CompositeQueue::gather`'s
  whole reset branch is dead. Either delete the three of them, or say what is
  meant to call `clear` and why nothing does.

## Include blocks out of the order the tree states

`AGENTS.md` fixes the order: own header, standard library, other libraries,
`<components/...>`/`<apps/...>`, then quoted local headers. `.clang-format`
preserves the blocks, so nothing checks this.

- [ ] `components/rtx/texturebuilder.cpp:15-22` — `<components/debug/debuglog.hpp>`
  and `<components/resource/imagemanager.hpp>` come after the quoted locals.
- [ ] `apps/openmw/mwrender/renderingmanager.cpp` — `<components/weather/precipitation.hpp>`
  was added inside the quoted-local block, after `"vismask.hpp"`.

## Helpers with external linkage that every neighbour keeps internal

- [ ] `components/rtx/lightbuilder.cpp:549-583` — `lightPhase`, `band` and
  `flame` are defined at `Rtx` namespace scope and declared in no header, so
  they have external linkage. Every other helper in the file sits in an
  anonymous namespace. Move them into one.
- [ ] `components/rtx/lightbuilder.cpp:31-113`, `:115-208`, `:210-240` — three
  separate anonymous namespaces in one file, with `Rtx`-scope definitions
  between the second and the third. One block, or a comment saying why not.

## One fact derived twice across the two hosts

- [ ] `apps/openmw/mwrender/renderingmanager.cpp:809` computes the rain
  intensity from `mSky->getRainRipplesEnabled()` and
  `mSky->getPrecipitationAlpha()`; `components/rtx/frameworld.cpp:14-17`
  computes the same number from `Weather::Precipitation`. Both read the same
  object by different routes. `Rtx::rainOnWater` is already the shared
  spelling — call it from `RenderingManager::update` too.

## Smaller things

- [ ] `components/rtx/lightbuilder.cpp:448-450` — a blank line between
  `weatherIndex`'s signature and its opening brace.
- [ ] `components/rtxvulkan/vulkanrenderer.hpp:216-219` — `static constexpr
  VkFormat sTargetFormat` is declared between `mInstance` and `mDevice`, inside
  a member list whose opening comment states that declaration order is
  destruction order. Move it above the data members.
- [ ] `components/rtxvulkan/vulkanrenderer.cpp:124-125` — no blank line between
  `deviceExtensionsFor` and `hasSea`'s doc comment.
- [ ] `components/rtx/spanallocator.cpp:37-42` — the best-fit search ranks holes
  by `hole->mCount`, not by what is wasted after the block alignment `place()`
  applied. With `mBlock` set, a smaller hole can waste more than a larger one.
  Rank on `taken.getEnd() - hole->mOffset`, or say why the hole's own size is
  the right key.
- [ ] `components/rtx/sceneextractor.cpp:337-338` — `descend` recognises
  `osg::Sequence` with `std::strcmp(node.className(), "Sequence")` where every
  other gate on this walk uses `isFrom(node, "<library>")`. Both are cheap; one
  spelling would read better and would put the class test beside the library
  test in `nodelibrary.hpp`.
