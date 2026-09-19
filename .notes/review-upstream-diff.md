# Review: the whole diff against `upstream/master`

Delete an item when you address it. Delete a heading when its last item goes. The `[RTX]`
settings pages and their translations are out of scope by decision and are not listed.

Scope: `git diff upstream/master HEAD`, 752 files. Tests are ignored by the review's rule;
they are a third of the lines. Review the gathered rasterizer with
`git diff -M20% --color-moved=dimmed-zebra`, or `glrenderer.cpp`, `glworld.cpp`,
`gloffscreenview.cpp` and `precipitation.cpp` read as 2,600 new lines.

## The rasterizer was changed where the rule says it is the path not taken

Each of these edits an upstream file so that the played GL game behaves differently from
upstream. The fork's rule allows a lift that the rasterizer reads unchanged, and nothing else.
Every one is also diff a reviewer must read as a behaviour change rather than as a move.

- [ ] `components/sdlutil/sdlvideowrapper.{hpp,cpp}` (14+/24−): `VideoWrapper` lost its
      `osgViewer::Viewer` and `setSyncToVBlank`. The GL host's vsync now lives in `GlRenderer`.
      Add a viewer-free constructor beside upstream's instead of rewriting the class, and leave
      `setSyncToVBlank` where upstream calls it.

## One truth, two sources

- [ ] `mwrender/rtx/rippleemitters.hpp` documents itself as "a copy of `RippleSimulation::update`'s
      rule" (`isUnderwater && !isSubmerged || isWalkingOnWater`, 12 units per particle). Lift the rule
      into one function both call, the way `components/sky/` was lifted, or have `RippleSimulation`
      produce the impulses and the RT path read them from the frame.

## The seam carries what only one host or one renderer reads

`MWRender::Renderer` (`renderer.hpp`, 399 lines, about forty members) is the one interface both
renderers implement. Members that only the harness or only the trace uses make the GL side carry
stubs and make the interface read as the RT renderer's.

- [ ] `PostProcessor` is GL-only and never built under RT, so `mwlua/postprocessingbindings.cpp`
      (20+/14−), `mwlua/debugbindings.cpp` and `windowmanagerimp.cpp` each grew a null check on
      `getPostProcessor()`. Answer once at the seam (a `Renderer::getPostProcessor()` that the RT
      renderer answers with "none", or bindings registered only by the GL host) and take the four
      checks out of upstream's files.

