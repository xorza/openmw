# What the rasterizer draws of vanilla content and the trace does not

A review of `apps/openmw/mwrender/` against `components/rtx*/` and `apps/openmw/mwrender/rtx/`,
September 2026, with the three shipped archives parsed for the counts. Of the fifteen gaps the
review found, thirteen are closed and the tree says how: the surface description honours
`OVERRIDE`, carries a texture's wrap, the environment and dark roles, the blend kind and the
ambient override; additive meshes are gathered where the flames are; billboards face the eye
that is looking; the water remembers what walked through it; a roof keeps the rain off; the moon
takes a script's paint, `tsky` hides the sky, the sun glare fader washes the picture; and the
ground exists for the intersector. The two that remain are below, each behind a decision, each
with its design, each to be measured before it stays.

**Every count below is a count of vanilla files.** The three BSA archives hold 7,320 NIF files, and
the numbers come from a parse of their records, not from what a scene report happened to show.
Detail, decal, gloss and bump maps occur in none of them; no vanilla shape turns the depth test off;
two shapes carry vertex alpha below one. Those are not gaps and nothing below reads them.

## What is still open

| gap | where it is decided today | vanilla content it costs |
|---|---|---|
| arms trace at the world's field of view | `OverrideFieldOfViewCallback` is a cull callback | `first person field of view` when it differs from `field of view` |
| debug modes draw nothing | their nodes hang above the scene root and are lines | `tcg`, `tpg`, `tnm`, actor paths, recast mesh |

## What the redesign holds to

The rules are `AGENTS.md`'s, and each part below is checked against them:

- **The decision comes over, the mechanism stays behind.** A field of view is a statement about
  what the world looks like. The depth clear is how a rasterizer got a triangle onto a screen
  and stays where it is.
- **One path through a shader.** Nothing below adds a branch per lane. A new term is a factor of
  nought where the content did not ask for it.
- **Allocation is a metric on the frame path.** Every new per-frame list is a persistent buffer
  `clear()`ed and refilled.
- **One answer shared with the rasterizer.** Where the game already states a fact, the trace reads
  it from where the rasterizer reads it.
- **No upstream file changes.** Every edit lands under `components/rtx*/`,
  `apps/openmw/mwrender/rtx/`, `apps/components_tests/rtx*/` and `files/rtx/`.
- **The gate is the gate.** Every step ends with the filtered tests, `shot --views=all
  --against=<before>` naming what moved, and `rtx.sh debug repeat`.

## Part K. The arms have an eye of their own

`NpcAnimation` draws the first-person arms under `Mask_FirstPerson` with two decisions the
rasterizer makes in a cull callback and a render bin: their own field of view, and a depth clear
so they stand in front of everything. Both defaults leave the picture as it is today (the two
fields of view ship equal; vanilla's own arms clipped), so this part lands last and is the one to
decide by looking.

- `EyeState` gains `mArmsFieldOfView`, read in `describeEye` off `mFirstPersonFieldOfView`.
- `WorldMirror` reports whether any placement of class `FirstPerson` stood this frame;
  `RtxRenderer::aim` builds `constants.mArms` — a second `Camera` at the same origin with that
  field of view, and a flag — only then.
- `visibility.rgen`, when the flag is set, traces `rayAt(frame.mArms, pixel)` on
  `MASK_FIRST_PERSON` alone before the world's ray. A hit is the pixel's surface and the world's
  trace is skipped; the pane peel, the medium walk and the sprites run along the arms' ray to the
  arms' distance. `motionOf` and `clipDepth` take the camera they project through, so the arms'
  motion and depth are the arms' camera's.
- `MASK_FIRST_PERSON` leaves the world's `mRayMask`; it is the arms trace's alone.

One trace per pixel against a mask that admits a handful of instances. Measured before it stays.

## Part L. Debug lines are drawn, not traced

The navmesh, the pathgrid, the actor paths and the recast mesh are `osg::Geometry` under
`Mask_Debug` on the world root, lines and triangles, unlit and blended. A ray cannot meet a line.
A `LinePass` beside `GuiPass` — the same graphics-pipeline shape, a per-frame vertex buffer
refilled from a `DebugWalk` over `Mask_Debug` nodes of the world root, depth-tested against the
traced depth at the nearest texel — draws them over the frame before the GUI. Last, and only when
wanted: it is a tool and not the picture.

## Implementation plan

### The arms' eye (Part K) and debug lines (Part L)

Each behind a decision, each with the design above, each measured before it stays.

- Part K: `camera.hpp`, `sceneframe.hpp`'s reader in `rtxrenderer.cpp`, `visibility.h`,
  `visibility.rgen`, `reproject.glsl`. Test: a traced test with the arms' camera at a wider field
  of view than the world's, asserting an arm's pixel lands where the wider camera puts it and the
  world behind it where the narrower one does. Verify: `shot` in first person with the two
  fields of view apart; `bench` against a run without the flag, for the trace's cost.
- Part L: `linepass.{hpp,cpp}`, `shaders/line.{vert,frag}`, a `DebugWalk` in
  `apps/openmw/mwrender/rtx/`. Test: a walk over a root holding one `Mask_Debug` line places
  two vertices and nothing under any other mask. Verify: `view` with `tpg` on.

## What is not done, and why

- **A real reflection on env-mapped surfaces.** The content states a sheet, and the sheet is what
  is drawn — `traversal.glsl`'s `resolveFor` reads it by the eye's own basis, as the rasterizer's
  `SPHERE_MAP` does.
- **Billboards under the ring's templates.** No vanilla static carries one; the walk turns them
  and the ring does not.
- **The vanilla particle-ring ripples.** They are the rasterizer's answer without a water shader;
  the trace's water is the shader's, and the field `RipplePass` steps is the shader's own.
- **`first person field of view` and the depth clear by default.** The defaults draw what is
  drawn today. See K.
- **Detail, decal, gloss and bump maps, `NiSpecularProperty`, `NiStencilProperty`.** None occur
  in the shipped files, or the loader already refuses them.

## What it costs

- Part K, per frame: one trace per pixel against a mask a handful of instances carry, on frames
  the arms stand in.
- Part L, per frame: a walk over the debug nodes and one graphics pass, only while a debug mode
  is on.
