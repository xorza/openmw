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

**And it is not the renderer either.** Giving `verify` a renderer of its own per view leaves
`island-crossing` differing by as much as 9 of 255. So cross-view state in the renderer is out, and
with it the whole premise this section started from.

**What that leaves is the `World` the views share.** `verify` and `bench` both stage through
`StagedWorld`; `shot` does not, and `shot` is byte-identical. `bench --views=island-crossing` is
identical too — and it renders the same frame 0 that `verify` does. The difference between them is
that `verify` walks sixteen views into one `World` in a fixed order, so what that view is staged
into depends on what the fifteen others loaded and dropped.

**Next, in order.**

1. **Run `verify` on that view alone**, which needs the view filter `bench` has and `verify` does
   not. If one view in isolation is reproducible, the shared `World` is the cause and the search is
   what a cell load leaves behind in it.
2. **If it still differs alone**, the cause is in `StagedWorld` for that view, and `bench` reaching
   the same camera reproducibly says the difference is in the staging rather than the frame.
3. **Either way the fix is not isolation.** The game loads cell after cell into one world for hours;
   whatever this is, a player meets it.

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

**The two shading paths agree, so it is not shading.** Measured: `shot` at that camera reads a linear
mean of 0.03434 with 46 of its 133 terrain materials baked, and **the same 0.03434** with every one
of them forced onto the layer stack. `CompositeQueue`'s claim that a chunk waiting for its bake looks
like one that has it holds, at least here.

**What is left is what the ring holds.** The two are not looking at the same world: `shot` has 8653
instances, all of them placed, and 890 materials that are not terrain; `bench` around there has
19040 instance slots with 8455 placed — the rest are gaps departed cells left — and 1281.

**What the pictures show.** `bench`'s frame is black to the eye with one faint horizon curve;
`shot`'s at the same camera is lit ground filling the frame. Every ray hits in both, and the sea is
at z −0.5 with the camera at 3000, so what is black is the ground itself.

**So a terrain surface is being shaded with an albedo of nearly nothing, in `bench` and not in
`shot`, on a path the two agree about.** What is left that `bench` does and `shot` does not is
churn: 19040 instance slots with 8455 placed, against 8653 all placed, and a texture array that has
had slots freed by departed cells and taken over.

**Next, in order.**

1. **Follow one hit.** Take a pixel in the black, read back which instance it hit, and read that
   instance's material and its layers' texture slots. Compare with the same pixel under `shot`.
2. **If the slots differ, ask who freed them.** A material naming a slot a departed cell gave back
   and a new texture took over is the shape of it, and `dropTextures` and the array's reuse are
   where that would live.

## Order

3 next, because the test it wants is worth having whatever the answer is. Then 1, which is the
longest and the only one with a step that could show its own premise wrong.
