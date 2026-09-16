# The queue owns what it may still read: graveyard and Vulkan API review

Delete an item when it is addressed, and a step when it has landed. Delete the file when both
lists are empty.

Scope: `components/rtxvulkan/graveyard.*`, `commands.*`, `timeline.*`, `readstamp.hpp`,
`buffer.*`, `image.*`, `memory.*`, `accelerationstructure.*`, `structurestorage.*`,
`framering.*`, `device.*`, and every site that buries, keeps or destroys a device object.

What was read and is not a finding: every `VkResult` is checked or handled by name; every handle
but the surface is an `Owned`; the memory allocator recycles its blocks and asserts them empty
at the end; the timeline is the one clock and only the pool advances it; the presenter's
semaphores are per acquire and per image for the reasons its comments give. No leak path was
found. Every item below is about making the safe path the only path, or the only path cheaper.

## Findings

### A device object can be destroyed while a submit still reads it, and nothing says so

The graveyard is the one correct way to let go of an object a submit may still name, and no
destructor enforces it. `Buffer` carries a `ReadStamp` and asserts it for host writes, and its
destructor is defaulted. `Image`, `Texture` and `AccelerationStructure` carry no stamp. Every
resize, scene rebuild and teardown destroys images and structures by assignment after a
`drain()`, and only the call order says that is safe. `BlockedBuffer::reserve` once destroyed its
old table on the floor and lost the device; the next bug of that kind is as silent.

- [ ] `Buffer`'s destructor and its move-assignment over a held buffer assert `isIdle()`. A buffer
      named only for the next submit passes, as `ReadStamp::isIdle` says, so a `Batch` discarded
      while unwinding passes too.
- [ ] `Image`, `Texture` and `AccelerationStructure` cannot be stamped at every hand-out — a
      bottom level is read by every top-level build without being named again — so their
      destructors assert the one thing always true of a safe destruction: the queue is idle, or
      the graveyard is the one destroying.
- [ ] `Device::waitIdle` waits the queue out and tells the timeline nothing. After it,
      `hasFinished` is false for everything the queue has passed, `Graveyard::collect` frees
      nothing, and the renderer needs `Graveyard::clear` — "destroy whatever the timeline says",
      unsafe anywhere but after a device idle, guarded by its comment. The renderer's own
      destructor is one of the two callers.
- [ ] `Timeline::next` is public and one caller advances the clock. A second caller would put the
      clock ahead of the queue, the bug the class doc names.
- [ ] `growTo` returns the displaced buffer for the caller to bury, and fifteen callers write
      `mGraveyard.bury(growTo(...))`. `[[nodiscard]]` is the contract's shadow: the function that
      displaces should bury. The same for `std::exchange(mTextures[slot], Texture(...))`,
      `std::exchange(row.mStructure, made)` and the top level's replacement.
- [ ] `CommandPool::reset` is documented as "nothing may be in flight and nothing deferred" and
      asserts the second half only.

### Five holders of "what a submit may still be reading"

The graveyard holds what the queue may still read, stamped with the next submit. Beside it:
`Batch::keep` holds staging until the batch's submit, `CommandPool::mDeferredStaging` holds it
again once the batch is deferred, `VulkanRenderer::mDyingScenes` is a graveyard for scenes with
its own stamp and an ordering comment against the other, and `GuiTextures::mRetired` is a list
of images held back from the graveyard because a burial at `drop` would be stamped before the
batch that reads them is deferred. Each is right alone. Together they are five places one rule is
kept, and `BlockedBuffer::reserve` uses `keep` as a burial because it happened to be in hand.

- [ ] `Batch::keep` means "what this recording reads, which nothing may destroy before it runs".
      Make it that for images as well as buffers, and have the batch bury everything it kept when
      it ends: at `defer` under the next submit, which is the one it rides; at `flush` after the
      wait. Then the pool's deferred staging, `GuiTextures::mRetired` with its deferral to
      `startFrame`, and `BlockedBuffer`'s special case go.
- [ ] `mDyingScenes` is the graveyard's rule kept a second time. Bury a scene as anything else
      is buried and free it last, which is the order the comment on `DyingScene` asks for. The
      graveyard is then collected once a wait rather than twice a frame.
- [ ] `Graveyard::bury(VkQueryPool)` takes a raw handle out of an `Owned` through `release()`,
      the one use of `release` in the tree, and destroys it with a `vkDestroyQueryPool` of its
      own. Take the `QueryPool`; then every burial but the pool's own command buffers is an RAII
      object.
- [ ] `bury(Buffer&&)` tests `getHandle() != VK_NULL_HANDLE` where the other four test
      `isEmpty()`.

### The graveyard and the pool are passed down the stack beside the device that could hold them

`Device` owns the queue's clock and its memory and hands both out through a const reference.
The command pool and the graveyard are the queue's in the same sense — one each, made right
after the device, used by everything that submits or lets go — and instead they are members of
`VulkanRenderer` threaded through twenty-four `Graveyard&` and about thirty `CommandPool&`
parameters and members. `Presenter` holds a `Graveyard&` only to hand it to `mPool.submit`.
`SlotTable::open` takes a graveyard pointer beside the device. The graveyard needs the pool to
free command buffers and the pool needs the graveyard to bury deferred batches: a pair with no
owner between them.

- [ ] `Device` owns the `CommandPool` and the `Graveyard`, reached as `getTimeline` is. Every
      `Graveyard&` and `CommandPool&` member and parameter goes, `CommandPool::submit` loses its
      `Graveyard&`, and `outgrow` loses its last parameter and moves beside `growTo`.
- [ ] "Collected once per wait, which is where what it knows changes" is not what happens: the
      waits `ReadStamp::waitIdle` makes for a table's copy advance the timeline and collect
      nothing, and a frame's wait collects twice. Collect where the wait is, once.

### Transient device objects are made and buried per arrival instead of recycled

The allocator recycles its blocks and a buried buffer's range goes back to it, so an arrival
costs no `vkAllocateMemory`. It still costs a `vkCreateBuffer`, a bind and a destroy per
transient object, and a command-buffer allocate and free, under a pool created with
`RESET_COMMAND_BUFFER_BIT` for exactly the reuse that never happens. The tree's rule is that a
loader is a persistent object owning its buffers.

- [ ] `Batch::stage` makes an eight-megabyte staging `Buffer` per batch and the pool buries it at
      the submit. Keep the blocks in a ring whose entries carry the value of the submit that
      read them, taken again once the timeline has passed it, made only when none is free.
- [ ] `CommandPool::begin()` allocates a one-shot command buffer per `Batch` and the graveyard
      frees it. Keep a free list under the pool, take from it first, and have the graveyard hand a
      finished buffer back to it.
- [ ] `BottomLevelStore::build` makes an `arrived` positions buffer and a `scratch` buffer per
      build and hands both to the batch. Both are the store's to keep and `outgrow`, as the top
      level's scratch is in `SceneAcceleration`. Safe, because a batch begins with the barrier
      that orders it after everything earlier on the queue.
- [ ] `uploadBuffer` makes a staging `Buffer` per upload beside `Batch::stage`, whose blocks were
      introduced to stop exactly that. Write it as `Buffer::deviceLocal` plus `stageInto`.

Not recommended: a persistent read-back buffer for `Image::read`. The harness reads a frame
once a frame under `--hashes`, and the cost there is the submit-and-wait, not the buffer.

## Redesign

**The queue owns its clock, its pool and its graveyard, and a device object dies in one of two
ways: the queue is idle, or the graveyard frees it after the timeline has passed its last
reader. A debug build aborts on any third way.**

- `Device` gains the `CommandPool` and the `Graveyard`, after the `Timeline` and torn down
  before the `MemoryAllocator`, handed out through the const reference everything already holds.
  `Device::waitFor(value, what)` is the one wait: the timeline's, then the graveyard's collect.
  `Device::waitIdle` advances the timeline to what was submitted and collects. `Timeline::next`
  belongs to the pool.
- The graveyard is the one retirement path. It takes every RAII object the backend has —
  `Buffer`, `Image`, `Texture`, `AccelerationStructure`, `QueryPool` — the pool's own command
  buffers, and `std::shared_ptr<void>` for anything else, which is how a scene retires. It frees
  in the order the destructors need and the scenes last. `clear` goes; `collect` after a wait is
  the whole of it, and the destructor asserts it holds nothing once the queue is idle.
- What displaces buries. `growTo` and `outgrow` bury the buffer they displaced;
  `Graveyard::replace(held, made)` buries what it exchanges out. A caller never holds a displaced
  object.
- What a batch reads, the batch buries. `Batch::keep` takes buffers and images and buries them
  when the batch ends — at `defer` under the next submit, at `flush` after the wait. The pool
  carries deferred command buffers only.
- Destruction asserts. `Device::mayDestroy()` is "the queue is idle, or the graveyard is
  reaping". `Buffer` asserts `isIdle() || mayDestroy()`; `Image`, `Texture` and
  `AccelerationStructure` assert `isEmpty() || mayDestroy()`. Debug only, as every assert on the
  frame path is.
- The pool recycles. One-shot command buffers come off a free list the graveyard refills.
  Staging is a ring of blocks the pool owns, each stamped by the submit that read it; a batch
  takes blocks whose stamps have passed and stamps them again when it ends. The build's scratch
  and its arrival buffer are the store's, grown and never made per arrival.

## Implementation steps

Each step builds, passes `components-tests --gtest_filter='Rtx*'`, and ends with
`rtx.sh debug gate`. Steps 3 and 4 end with `shot --views=all --against` the pictures of
step 0 as well, since they move burials; step 6 ends with `bench --views=island-crossing`
against step 0's `place ms` and `walk ms`, p99 and worst.

0. **Baseline.** `shot --views=all --map --doll=fargoth --upscale=off --filter=false
   --exposure=1 --validation=off --out=<dir>` and a `bench --views=island-crossing
   --window=false` for the numbers step 6 is read against.

1. **The device owns the pool and the graveyard.** `Device` gains `std::unique_ptr<CommandPool>`
   and `std::unique_ptr<Graveyard>`, made after the timeline; `~Device` resets them before the
   allocator. `getPool()` and `getGraveyard()` return references from a const device, as
   `getTimeline` does. Remove every `Graveyard&` and `CommandPool&` member and constructor
   parameter in the backend (`grep -n 'Graveyard&\|CommandPool&' components/rtxvulkan/*.hpp`
   lists them), reaching both through the `const Device&` every class holds. `CommandPool::submit`
   drops its `Graveyard&`; `SlotTable::open` drops its graveyard; `outgrow` drops its last
   parameter and moves to `buffer.hpp` beside `growTo`. `VulkanRenderer` loses `mPool` and
   `mGraveyard`. Tests that built a pool or a graveyard use the device's. Mechanical, and the
   largest step by diff.

2. **The clock learns every wait.** `Timeline::markIdle()` sets what is finished to what was
   submitted; `Device::waitIdle` calls it, then collects. `Device::waitFor(value, what)` waits
   on the timeline and collects; `FrameRing::finishOldest`, `Timeline`-level waits in
   `ReadStamp::waitIdle` (which takes the device) and `CommandPool::endAndWait` go through it.
   `Graveyard::clear` is deleted; `VulkanRenderer::drain` and `~VulkanRenderer` become
   `finishDeferred`, `finishAll`, `waitIdle`. `~Graveyard` asserts it holds nothing.
   `Timeline::next` becomes private with `CommandPool` a friend. `CommandPool::reset` asserts the
   timeline idle.

3. **What displaces buries.** `growTo` and `outgrow` bury through the device and return `void`
   and `bool`; the fifteen `bury(growTo(...))` sites become `growTo(...)`.
   `Graveyard::replace(T& held, T&& made)` for the texture slot, the compacted structure and
   the top level. `[[nodiscard]]` goes with the return.

4. **One graveyard.** `bury(QueryPool&&)` replaces the raw overload and `release()` at its one
   site. `bury(std::shared_ptr<void>&&)` is added and freed last; `mDyingScenes` and
   `buryDyingScenes` go, a retired scene is `bury(std::shared_ptr<void>(std::move(scene)))`
   at the site that stamped it. `bury(Buffer&&)` tests `isEmpty()`. `Batch::keep` takes
   `Buffer&&` and `Image&&` and holds both; `Batch::defer` buries everything kept, then hands the
   pool the commands alone; `Batch::flush` buries after the wait. `CommandPool::defer` loses its
   staging parameter, `mDeferredStaging` goes, `forgetDeferred` clears the commands only.
   `GuiTextures::drop` keeps the image on its batch, and `mRetired` and its loop in `startFrame`
   go. `BlockedBuffer::reserve` buries its old table through `replace`. The check under the
   layers is the test of this step: a burial stamped too early is a validation message at the
   submit that reads it.

5. **Destruction asserts.** `Graveyard::freeThrough` raises a reaping flag for its duration;
   `Device::mayDestroy()` is the timeline idle or that flag. `Buffer`'s destructor and
   move-assignment assert `isIdle() || mayDestroy()`; `Image`, `Texture` and
   `AccelerationStructure` assert `isEmpty() || mayDestroy()`. Run the gate under `asan` as
   well as `debug`, and `bench --views=island-crossing` once, because an arrival is where a
   destruction that should have been a burial would fire.

6. **The pool recycles.** `CommandPool` keeps a free list of one-shot buffers; `begin()` takes
   from it before allocating; the graveyard's `bury(VkCommandBuffer)` hands a finished buffer
   back to it. A `StagingRing` under the pool owns blocks of `sStagingBlock`, each with the
   value of the submit that last read it; `Batch::stage` takes blocks the timeline has passed
   and appends into them, and a batch stamps its blocks when it ends. `BottomLevelStore` keeps
   `mArrived` and `mScratch`, `outgrow`n per build. `uploadBuffer` becomes `Buffer::deviceLocal`
   and `stageInto`. Read the arrival frames of the island crossing against step 0: the change
   is the `place ms` worst frame, and it must not rise.
