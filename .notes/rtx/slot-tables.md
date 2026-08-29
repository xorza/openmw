# The per-copy debt: what it is, why it keeps failing, and what replaces it

Five bugs in one mechanism in one session, every one of them silent. This is the account of why,
and the design that removes the class rather than the instances.

## What the mechanism is

Two frames may be in flight, so every table a frame writes has one copy per frame slot. A copy is
written incrementally: rather than rewrite the whole table, each copy carries a **debt** — the rows
it has not been told about since it was last written. `RowDebt` holds it, `owe` adds to it, `settle`
clears it, and a placement pays the debt of the copy it is writing.

The debt is filled from the scene's own change lists — `getMoved()`, `getSettled()`,
`getDeformed()`, `getWrittenMaterials()` — which the walk fills and `advancePlacement` rotates.

## What is actually in the tree

**Five independent ledgers**, each with its own subscription, its own settle point, and its own
whole-write escape hatch:

| ledger | owes | escape hatch |
| --- | --- | --- |
| `SceneBuffers::mRowsOwed` | settled + moved | `outgrow` (doubling) |
| `SceneBuffers::mMaterialsOwed` | written materials | `outgrow` (doubling) |
| `SceneBuffers::mNormalsOwed` | deformed | block growth |
| `SceneAcceleration::mRowsOwed` | settled + moved *(as of today; it was moved alone)* | exact sizing |
| `SceneAcceleration::mPositionsOwed` | deformed | none |

**A sixth staleness variable that is not a ledger at all**: `Tables::mMaterialCount`, which exists
only to decide whether the sentinel row past the materials has moved in this copy.

**Two tables that opted out**: `mLayers` and `mMasks` are written into *every* copy at once, because
"a chunk arriving is an arrival the caller waited every frame out for". So the tree already carries
two answers to the same question.

**Three consumers of the same change lists that must agree**: `updateInstanceRecords`,
`SceneBuffers::place` and `SceneAcceleration::place`. Two of them read settled and moved; the third
read moved alone until today.

**Nothing checks any of it.** There is no assert and no test anywhere that a copy holds what the
host mirror holds.

## The four ways to be silently wrong, and the four that happened

The debt is *what the scene announced*, replayed per copy. That makes correctness depend on four
separate things, none of them local, none of them checked:

1. **Every consumer subscribes to the same lists.** — `SceneAcceleration` subscribed to `getMoved()`
   alone while `SceneBuffers` took both. Terrain a frame stale.
2. **The lists are still current when each consumer runs.** — `advancePlacement` rotates moved into
   settled. `StagedWorld::mirror` ran it *between* the walk and the hand-over, so `getMoved()` was
   empty at every placement. `PosedActors::advanceTo` did the same, on the path actors take.
3. **Every writer settles exactly once.** Miss one and the debt leaks; add one and rows are lost.
4. **The escape hatch agrees with the debt about when a whole write happened.** —
   `SceneAcceleration::place` skipped the top level when *the scene* had not moved, which stopped
   being the same question as whether *this copy* owed rows.

Four failures, four causes, one mechanism. The fifth is still open: 26 frames of 600 still swing,
where forcing both frames onto one copy gives 5.

**None of them produced an error.** No validation layer fires, no assert trips, no test fails. The
symptom is a picture, one frame, sometimes.

## Why the design invites it

The debt describes **an event** — "the scene moved these rows" — and the thing that has to be true
is **a state**: *this copy differs from the host mirror in exactly these rows*. The code converts
the second into the first by hand, at five sites, and every conversion is a chance to disagree.

The mirror already exists. `SceneBuffers::mInstanceRows` is documented as "the one answer every copy
is written from"; `SceneAcceleration::mRows` is the same thing. But it is filled **lazily, beside
the copy write** — `placeRow(at)` computes the row and writes the copy in one step, and only for
rows the debt named. So the mirror is not a source of truth; it is as stale as the copies are.

That is the root of it. There is a natural single source of truth and the design does not use it.

## What replaces it

**Track staleness where the write happens, not where the change is announced.**

```cpp
/// A host-side table and one device copy per frame in flight. A copy is only ever behind by
/// writes it has not taken, and taking them is the only way to read it.
template <class Row>
class SlotTable
{
public:
    Row& write(Index at);                        // marks `at` owed by every copy
    void resize(std::size_t rows, Graveyard&);   // marks everything owed by every copy
    bool owes(std::uint32_t slot) const;
    void sync(std::uint32_t slot, Graveyard&);   // writes this copy's owed set and clears it
    VkBuffer getHandle(std::uint32_t slot) const;

private:
    std::vector<Row> mRows;
    std::array<HostBuffer, sFrameSlots> mCopies;
    std::array<RowDebt, sFrameSlots> mOwed;
};
```

Each of the four failure modes stops being expressible:

1. **One subscription.** There is nothing to subscribe to — a copy is behind because `write` was
   called, not because a list said so. Two tables cannot disagree about which list to read.
2. **No lifetime.** The owed set is not a view onto a scene list, so when `advancePlacement` runs
   no longer changes any answer. Both epoch bugs become impossible rather than fixed.
3. **`sync` clears exactly what it wrote**, in the same function, on the same object.
4. **`resize` marks everything owed**, so growth and debt cannot disagree. `owes(slot)` is the
   early-return question, asked of the thing that knows.

`mMaterialCount` disappears: the sentinel is a row like any other.

**Two types, not one.** `mNormals` and `mPositions` are `BlockedBuffer`s addressed by mesh range,
so they want a `SlotBlocks` sibling with `write(range)` rather than `write(index)`. Five ledgers and
one ad-hoc counter become two types with one rule each.

**What it costs.** `write(at)` marks the row owed by every copy, which is one push per copy per
changed row where today a placement pushes one list per copy. That is the same order. The real cost
is that the mirror must be filled eagerly — `placeRow` becomes a `write` at the point the record
changes rather than at the point a copy is paid — which moves work from the placement to the walk
and does not add any.

## What was built

**`SlotTable<Row>`** — host rows, one device copy per frame slot, one owed set per copy.
`write(at)` marks the row owed by every copy; `resize(rows)` owes what it appended and nothing else,
because a row keeps its offset; `sync(slot, graveyard)` pays one copy and clears it; `owes(slot)` is
what an early return asks.

**`SlotBlocks`** — its sibling for a table whose truth lives elsewhere. A mesh's vertices are the
scene's, so the caller passes `sync` a `fill(at, BlockedBuffer&)` and this says which runs are owed.
It keeps a plain list of runs rather than a `RowDebt`: there is no "everything" it could honour,
because nothing here knows how many runs there are or how to read one.

**One subscription for the whole system.** `updateInstanceRecords` names in an out-parameter every
slot it wrote, and both backends are driven by that one list. `SceneAcceleration::place` and
`SceneBuffers::place` no longer read `getMoved` or `getSettled` at all, so the two epoch bugs and the
subscription mismatch are not expressible in them.

**All five ledgers are gone**, and so is the sixth variable:

| was | now |
| --- | --- |
| `SceneBuffers::mRowsOwed` + `mInstanceRows` + `Tables::mInstances` | `SlotTable<GpuInstance> mInstanceTable` |
| `SceneBuffers::mMaterialsOwed` + `mMaterialScratch` + `Tables::mMaterials` + `mMaterialCount` | `SlotTable<GpuMaterial> mMaterialTable` |
| `SceneBuffers::mNormalsOwed` + `Tables::mNormals` | `SlotBlocks mNormalTable` |
| `SceneAcceleration::mRowsOwed` + `mRows` + `mInstances` | `SlotTable<VkAccelerationStructureInstanceKHR> mRowTable` |
| `SceneAcceleration::mPositionsOwed` + `mPositions` | `SlotBlocks mPositions` |

`RowDebt` now appears nowhere outside `slottable.hpp` and its own header.

**Eleven tests** in `apps/components_tests/rtx/slottable.cpp` ask the bookkeeping directly:
`HostBuffer` is write-combining and hands out no readable pointer, so what a copy holds cannot be
read back, but what it is *owed* can — and that is where every failure lived. Two of them exist
because the first cut had bugs of its own: `sync` doubled the buffer unconditionally and ran a
streaming bench out of device memory in twenty-two frames, and `SlotBlocks` inherited `RowDebt`'s
"owes everything" flag, which it has no way to honour.

**What it changed and what it did not.** The streaming blink is unchanged at 26 swings of 600, which
is what a redesign should do: the four point fixes had already taken it from 97, and this removes the
class rather than another instance. The cost is inside this box's run-to-run band — 68.8 fps against
67.4, where the same binary spans more than that between runs.

## What is left

The residual 26 swings are not this mechanism. §4's nineteen crossings are inside that count and are
the next thing to remove.
