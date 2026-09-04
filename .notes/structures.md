# Section 5 of the review, investigated

Every item read in the code, and the whole section measured. **The list is ranked wrong**: the two
most expensive containers in the tree are not in it, and eight of the nine items it does name cost
nothing a profile can see.

## What the measurement says

`profile.sh --view=vivec --seconds=10`, release, on-CPU `task-clock`, two runs. Shares of the
harness's CPU, attributed by the symbol each container's code was inlined into.

| container | item | run 1 | run 2 |
| --- | --- | --- | --- |
| `MeshResolver::mMeshes`, drawable identity | **not in section 5** | 1.88% | 1.96% |
| `MaterialResolver::mMaterials`, state-set identity | **not in section 5** | 1.39% | 1.20% |
| `SceneExtractor::mPlacements` | 5.4 | 1.24% | 1.22% |
| `PlacementTable` | — | 0.13% | — |
| `DistantLights::mCells` | 5.7 | 0.01% | — |
| `GpuBreakdown` | 5.5 | 0.01% | — |
| `TextureTable` | 5.1 | below 0.005% | — |
| `CompositeQueue` | 5.6 | below 0.005% | — |
| `TimeOfDaySettings` | 5.8 | below 0.005% | — |
| `LoadedCells` | 5.9 | below 0.005% | — |
| the per-slot flags | 5.2 | below 0.005% | — |

**Three `std::unordered_map`s cost 4.4% of the harness's CPU**, and everything else the section names
costs 0.02% between them. `osg::StateSet::getUniform`'s own `std::map` is 0.43% beside them, which
is OSG's and not this fork's.

The three are one problem and not three. Each is a map from a pointer — or from an integer folded out
of pointers — to a two-field record, asked once per drawable per frame and hitting almost every time.
libstdc++ chains its buckets and takes the hash modulo a prime, so each lookup is an integer division
and two dependent loads. Vivec reaches 99,299 drawables, which is about 300,000 divisions a frame.

## Corrections

**5.4's stated direction costs more than the map.** "Key on the placement identity itself" means
keeping the node path per placement — a vector of pointers each, against eight bytes now. And the
collision it guards against is remote: a 64-bit fold over 100,000 placements collides with a chance
near 10⁻¹⁰ a frame. The item is real, but it is real as *cost* and not as correctness, and the fix is
the same one the two identity maps want.

**5.5's stated direction does not fit the data.** "One `vector<double>` with a stride" needs every row
to be the same length, and they are not: a zone that first appears on frame 500 has 500 fewer samples
than one present from the start. What is real in the item is smaller — `mNames` copies a
`std::string_view` the backend owns into a `std::string`, once per zone.

**5.8 needs no go-ahead.** The review marked it as lifted shared code the rasterizer reads.
`mSunriseTransitions` is touched by `addSetting` and `getSetting` alone, both inside
`components/sky/`. Upstream's `mwworld/weather.cpp` holds a `TimeOfDaySettings` and reads its scalar
fields, and reaches the transitions only through `TimeOfDayInterpolator::getValue`, which is this
component's own template. An enum-indexed array behind an unchanged `getSetting` touches no upstream
file.

**5.1's direction trades a measured nothing for a reallocation hazard.** A string arena needs offsets
that survive growth and a way to give a freed name's room back, and it replaces
`VFS::Path::Normalized` — a type that carries what a path *is* — with an integer. Merging `mPaths` and
`mBaked` into one column is the smaller version of the same idea, and it rewrites `getTextures()`,
which is read in about sixty places including fifty test assertions. Neither buys a cycle. Refused
below.

## Part 1 — build now

These are shape and correctness. None of them needs a measurement, because none of them claims one.

### P1. `MyGUIRtx::RenderManager`'s clock becomes members

`rendermanager.cpp:182-183`. `update` keeps its timer and its last reading in function statics, so
the clock belongs to the process rather than to the manager.

**A latent bug and not a live one.** One `RenderManager` is made per process, in
`WindowManager`'s constructor, so nothing today has two of them. What the statics make impossible is
a second one — and what they make wrong is a manager destroyed and remade, which would hand MyGUI
the whole gap since the first one's last frame as a single frame delta and jump every animation in
the interface.

Two members and a first-frame guard. Three lines.

### P2. A duplicate-free slot list is a type

`scenedesc.hpp`. Four per-slot byte arrays, in two spellings, for two different jobs:

- `mDeformed` beside `mDeformedFlags`, and `mWrittenMaterials` beside `mMaterialWritten`, are **the
  same construction twice**: a list of slots kept free of duplicates by a byte that says whether the
  slot is in it. The invariant that the two agree is maintained in four places and stated in none.
- `mKeptMeshes` and `mKeptMaterials` are something else — a bitmap `markKept` builds and throws away
  inside one sweep — and they only need the spelling the rest uses.

```cpp
/// The slots of one table that something is true of, in the order they were named, each once.
///
/// **The list and the byte together, because neither is the answer on its own.** The list is what a
/// frame walks; the byte is what keeps a slot named twice in a frame from appearing twice in it.
/// Kept apart, the two fall out of step in the one direction nothing catches: a slot in the list
/// whose byte says it is not.
class SlotSet
{
public:
    void grow(std::size_t count);
    bool add(Index slot);
    void remove(Index slot);
    bool contains(Index slot) const;
    std::span<const Index> getSlots() const;
    void clear();
};
```

`SceneDesc` loses four members and gains two. `SlotChanges` is the same shape with two lists and
three states, and stays as it is — the two are neighbours, not one type.

### P3. `GpuBreakdown` stops copying the backend's names

`frametimes.hpp:137`. `GpuSpan::mName` is a `std::string_view` over a literal in the backend, and
`GpuTimer::Zone` already holds it as one. `mNames` copies each into a `std::string`.

`std::vector<std::string_view>` removes the copies and keeps the comparison by content, which is what
makes two call sites spelling one zone name land in one row. The header's own comment already says
the names are the backend's literals.

One line, and the `std::find` above it needs no change.

### P4. `TimeOfDaySettings` holds four constants in an array

`components/sky/timeofday.hpp:32`. `std::map<std::string, WeatherSetting>` for four entries — "Sky",
"Ambient", "Fog" and "Sun" — with four heap strings, four tree nodes and a string compare per
lookup.

```cpp
enum class DayPhaseOf
{
    Sky,
    Ambient,
    Fog,
    Sun,
};
```

`std::array<WeatherSetting, 4>` behind it. `getSetting(std::string_view)` stays, so upstream's
`getValue(hour, times, "Fog")` compiles unchanged, and it maps the name to the enumerator with four
comparisons instead of a tree walk. `"Stars"` names none of the four and keeps the documented
`{1,1,1,1}` — which is what `getSetting` already answers and what `sky/timeofday.cpp`'s test asserts.

An enum-taking overload beside it for `components/sky/`'s own callers, which know which they want.

### P5. `LoadedCells` is keyed on the grid position

`apps/rtxtool/cellscene.hpp:93`. `std::map<std::string, LoadedCell>` keyed on `"x,y"`.
`dropCellsOutside` builds a `std::set<std::string>` and nine `std::string`s to answer "is this cell
in the active grid", every time the camera may have crossed.

`std::map<osg::Vec2i, LoadedCell>` — `CellSquare` and `gridAround` already speak in positions, and
`cellAt` is where a name is spelled when one is wanted. The set becomes a comparison against the grid
bounds and the strings go entirely.

**Harness only, and on the crossing path** — which is the path this fork does care about, because a
crossing is the frame a player feels. `keyOf` stays for the log lines that name a cell.

### P6. `CompositeQueue::mFinished` is a vector indexed by slot

`compositequeue.hpp:186`. `std::unordered_map<Index, TerrainComposite>` keyed on a material slot,
which is a dense small integer into a table the queue already knows the length of.

A `std::vector<std::optional<TerrainComposite>>` grown with the material table. `find` becomes an
index. The rest of 5.6 is refused below.

## Part 2 — proposed, and deferred by the fork's own rule

### P7. One open-addressed table for the three hot maps

The 4.4%. `MeshResolver::mMeshes`, `MaterialResolver::mMaterials` and `SceneExtractor::mPlacements`
are one shape: a small key, a two-field value, one lookup per drawable per frame, almost always a
hit. What they pay for is libstdc++'s chaining and its modulo by a prime.

An open-addressed table with a power-of-two mask and linear probing turns the division into an AND
and the two dependent loads into one. It holds its entries inline, so an arrival stops allocating a
node. About 150 lines with its tests, one type, three users.

Two things it has to get right, and both are why it is not three lines:

- **The identity maps own their keys.** `Identity<T>` holds an `osg::ref_ptr`, and the comment on it
  says why — what it holds outlives the graph by one sweep. An open-addressed table moves its entries
  when it grows and needs a tombstone for `erase_if`, so the reference counting has to be right
  through both.
- **`mPlacements`' key has no free sentinel.** A folded hash may be any 64-bit value, so the empty
  and deleted states are a byte beside the key rather than a reserved value.

**Deferred, and the fork's own rule is why.** "No benching and no frame times until the renderer
draws everything the game has." The harness waits on the device at Vivec — `bench.txt` says so — so
4.4% of a CPU that is 0.60 cores busy may move no frame time at all. Land what is missing first.

**What would settle it**: after the renderer is feature complete, one profile. If the three still sum
above about 3%, build it and measure the frame; if the frame does not move, say so and stop. The A/B
method is the one section 2 established — interleaved legs, no cooldown, the clock and the
temperature read beside every result.

## Refused

**5.1, the texture table's seven structures.** Nothing measurable, and both versions of the direction
cost more than they buy: the arena replaces a type that carries a path's meaning with an offset and
brings a reallocation hazard, and the column merge rewrites `getTextures()` and its sixty readers.
What is genuinely two-of-a-kind — `mPathIndex` and `mBakedIndex` — is two lookups because a slot is a
file *or* a bake and never both, which is the invariant the class exists to keep.

**5.6's deques.** `mPending` and `mDone` are shared between the frame and the baker under a mutex. A
ring buffer over the request pool means a fixed capacity and a decision about what a full ring does
to a crossing that gathers dozens of chunks. Real risk, no measured gain. `mFinished` is P6 and is
the part that stands.

**5.7, `DistantLights::mCells`.** 0.01%, and the review says so itself. A flat grid has to follow a
moving eye, which is a shift or a ring index — real work for a hundredth of a percent.

## Implementation plan

Six steps, smallest first, each verified before the next. P7 is not in it.

### Step 1. P1 and P3

`components/myguirtx/rendermanager.{hpp,cpp}` and `components/rtx/frametimes.hpp`. Two independent
one-liners in two components.

Test: `frametimes.cpp` already has zone tests. `RenderManager`'s clock is checked by reading — nothing
in the tree drives MyGUI's frame event.

### Step 2. P6

`components/rtx/compositequeue.{hpp,cpp}`. One container, three call sites — `collect`, `finished`
and the reset in `gather`.

Test: `--gtest_filter=*Composite*:*Terrain*`.

### Step 3. P2

`components/rtx/slotset.{hpp,cpp}` beside `slotchanges`, then `scenedesc.{hpp,cpp}`.

Test: the deformed-list and written-material tests already exist in `scenedesc.cpp` and
`skinning.cpp`. Add one that names a slot twice in a frame and asserts the list holds it once — the
invariant the type exists for.

### Step 4. P4

`components/sky/timeofday.{hpp,cpp}`, and `sun.cpp` for the enum overload.

**No upstream file is edited**, and that is the claim the step has to keep: `git diff --stat` naming
`mwworld/weather.cpp` means `getSetting`'s string form did not survive.

Test: `--gtest_filter=*TimeOfDay*:*Sun*`. The existing `getSetting("Stars")` case is what pins the
name that names none of the four.

### Step 5. P5

`apps/rtxtool/cellscene.{hpp,cpp}`, `stagedworld.cpp`, and the tests that hold a `LoadedCells`.

Test: `--gtest_filter=*Crossing*:*PagedTerrain*:*Props*`, which are the tests that cross cells.

### Step 6. The review

Fold what is settled into `.notes/review.md` and delete this file, the way section 4 went.

## After every step

```
cmake --build build-debug
./build-debug/components-tests
CLANG_FORMAT=clang-format-14 CI/check_clang_format.sh
cd build-release && ./openmw-rtxtool verify --against=verify-ownership
```

**20 of 20 views identical is the bar.** It covers `components/` and the harness. It does not cover
`components/myguirtx/`, which step 1 touches: nothing in the tree drives MyGUI's frame event, so that
one is checked by reading.
