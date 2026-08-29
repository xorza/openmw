# The three open issues, and how to close them

What `ISSUES.md` lists, what has been ruled out for each, and the fix each one wants. Delete a
section when it is done, and the file when they all are.

## 1. `verify` differs from itself, and only `verify` does

`island-crossing` differs between two runs of one build by one to eight of 255 on 0.01% of its
pixels. Holding the exposure does not stop it and makes a second view join in.

**What that is not.** It is not the view and it is not the staged path:

| the same viewpoint, twice | result |
| --- | --- |
| `bench --views=island-crossing --hashes` | identical |
| `shot --view=island-crossing` | byte-identical PNGs |
| `verify` | differs |

`shot` stages its composites exactly as `verify` does, and `bench` reaches the same camera through
the streaming path. Both are reproducible. The two wall clocks a run used to read are fixed and
`verify` states its frame length like everything else.

**What is left is the one thing `verify` alone does: sixteen views through one renderer.** Something
carries from a view into the next. `setScene` resets the reference sum, the previous camera and the
slot bookkeeping, so what remains is device memory a departed scene wrote and a new one was handed,
the accumulator's images, and whatever the sea and fog passes keep across a scene.

**Next, in order.**

1. **Say whether it is the reuse at all.** Give `verify` a renderer per view. If it goes away, the
   cause is cross-view state and the search is bounded to what `setScene` does not reset. If it does
   not, everything above is wrong and the view itself has something.
2. **If it is the reuse, bisect what carries.** The order to suspect is memory a new scene is handed
   after an old one gave it back — which is uninitialised-read shaped, and matches a few thousand
   pixels differing by a few levels rather than a whole frame shifting.
3. **The fix is not a renderer per view.** That would hide it: the game reuses one renderer across
   every cell it ever loads, so whatever this is, a player meets it. A new scene must not be able to
   read what an old one wrote.

## 2. `bench` reports a frame result for fewer than half its frames

**Root cause, and it is exact.** `beginFrame` drains the ring when it is full:

```cpp
while (mFrame - mFinished >= sFrameSlots)
    finishOldest();
```

`finishOldest` returns a `FrameResult` and `beginFrame` discards it. The `finishFrame` that follows
then finds `mFinished == mFrame` and returns nothing, so the run's wait, GPU and hit rows are
sampled from whichever frames the caller happened to reach first.

**The fix.** A result belongs to the frame that produced it, and which call did the waiting is not
the caller's business. `finishOldest`'s result goes into a small queue — at most `sFrameSlots` can
be pending, since that is the cap on frames in flight — and `finishFrame` pops from it before
waiting for anything. `beginFrame` stops discarding; `finishFrame` stops missing.

That also fixes a smaller wrong thing: `finishFrame` currently means "wait for the oldest", and it
should mean "give me the next frame's report", which is what every caller wants it for.

## 3. One camera renders lit under `shot` and near-black under `bench`

At `19388,-27476,3000` looking at `20421,-25763,2300`: `shot --cell=2,-4 --albedo --upscale=off
--exposure=1` reads a mean of 42.0 of 255, and `bench --suite=streaming` reads 3.2 at its own frame
268.

**What that is not.** Measured, each on its own: not the double-buffered tables (one copy gives the
same reading), not the sweep, not the composite bake queue, not the upscaler, not the denoiser, and
not rays that miss — no pixel is a miss in either.

**What has never been compared is what the two have loaded.** `shot` stages a ring around the cell
it is given; `bench` has been flying and its ring is whatever `moveTo` last settled on. The same
ground can be a near chunk in one and a distant flattened chunk in the other, and those are two
different shading paths — `traversal.glsl` sums a layer stack for one and takes a single fetch for
the other. `Rtx::sCompositeFrom` is where they swap.

**This became tractable only now.** Both runs are reproducible, so the comparison is a diff rather
than an inference — which is what every earlier attempt at this lacked.

**Next, in order.**

1. **Compare the two scenes at that camera**, field by field rather than by hashing structs with
   padding in them: how many instances, which meshes, and for the chunk under the camera which
   shading path its material takes.
2. **If they take different paths, compare the paths.** The claim `CompositeQueue` makes is that a
   chunk waiting for its bake looks the same as one that has it — "the picture is right from the
   first frame and what the bake buys is the cost of that hit". Nothing tests that claim. A test
   that shades one chunk both ways and compares is what it wants, and it belongs beside
   `terraincomposite.cpp`, which already checks the bake against hand-computed values and never
   against the shader.
3. **If they take the same path**, the difference is in what the ring holds, and that is a streaming
   question rather than a shading one.

## Order

2 first: it is exact, it is small, and every measurement after it is read off a report that is
currently a sample. Then 3, because the test it wants is worth having whatever the answer is. Then
1, which is the longest and the only one with a step that could show the whole premise wrong.
