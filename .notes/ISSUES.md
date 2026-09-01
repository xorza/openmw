# Open issues

- Closing an `openmw-rtxtool view` window can abort instead of exiting. `~VulkanRenderer` calls
  `mDevice.waitIdle()` at vulkanrenderer.cpp:210, `checkVk` throws on the result, and a destructor
  that throws reaches `std::terminate` — so whatever the device reported is replaced by signal 6 and
  never printed. Seen once on `view --view=sadrith-mora` after a few seconds in the window.

- Opacity micromaps cost a cell crossing 340 ms and return no measurable trace time. Interleaved
  `bench --suite=exteriors`, three runs each way, comparing the median trace against the same binary
  with `buildMicromaps` returning at once: seyda-neen-ship +1.0%, seyda-neen-shore +1.1%, balmora
  +0.1%, vivec -1.2%, ald-ruhn -1.5%, sadrith-mora -1.4%, dagon-fel -0.3%. Every delta straddles
  zero.

  The reason is in the tally, not in the measurement: 96% of the microtriangle area a micromap
  covers still asks. `AlphaBounds` brackets a patch across every level of the mip chain, because
  `candidateStops` reads the mask at whatever level the ray's cone resolves — so a patch resolves
  only where the whole chain agrees about it, and Morrowind's masks are soft-edged. Raising
  `Micromap::sSubdivisionCeiling` is what would resolve more, and it costs `4^level`; lowering it
  from 5 to 3 cut classification 10.7× and halved the resolved share, with no change in trace time
  either way.

  What the crossing pays, flying the Bitter Coast at 12,000 units a second: 5.5 s of build over
  sixteen crossings against 0.5 s, and a worst frame of 1,280 ms against 238. The classification is
  the whole of it — 763 ms for 460 meshes on one crossing, of which the mask bounds are 24 ms.
  Before the arrival rule was fixed most of this work was skipped, which is why it was never
  visible.

  Three ways out, and they are not the same decision: take the classification off the frame the way
  `CompositeQueue` takes a terrain bake, and rebuild each mesh's structure when its micromap lands;
  make the micromaps resolve enough to pay, by cutting finer and by bounding the mask over only the
  levels the cutout test can read; or remove the path.

  An exact guard was tried and removed: a triangle whose whole box holds no certainly-material and
  no certainly-hole texel can have no piece that resolves, so its subdivision can be skipped. The
  tally came out identical to the byte and the cost did not move — nearly every triangle's box holds
  a certain texel of one kind or the other.

- One exterior renders differently depending on which cell the process staged before it, and every
  field of the scene it was handed is equal. `verify --views=balmora,seyda-neen-shore` against
  `--views=seyda-neen-shore` reports `differs: worst 8 of 255 on 0.43% of the pixels`. The pixels sit
  on one campfire west of the town, a second faint spot north of it, and a thin speckle over the
  grass between them.

  Only Balmora at -3,-2 and Vivec at 2,-10 do it. Ald-ruhn, Sadrith Mora, Dagon Fel, Vivec's
  canalworks, Addamasartus, the island crossing, Seyda Neen's own views and a second staging of the
  shore itself do not. Balmora and Vivec are the two nearest cells to Seyda Neen at -2,-9 of every
  one tried.

  Compared after staging Balmora at its own camera: the positions, normals, texture coordinates,
  indices, meshes, layers and masks by digest, and the materials, instances and sprite emitters
  element by element. All equal. `--distant-terrain=false` removes the difference.
  `--exposure=1`, `--delight=0`, `--distant-statics=false`, `sCompositeFrom` raised past every chunk,
  and the micromap bake disabled outright do not. `--gpu-validation` reports nothing.

- The instance count a `bench` reports is not reproducible. `--views=seyda-neen-shore --frames=1
  --warmup=0` gives 21,874 once and 21,887 twice, and `--seconds=1` gives 13,345, 13,350, 13,344 and
  13,344. The figure is the acceleration structure's own instance count, which the sweep compacts
  over frames, so a longer run reports a smaller number. `scene --view=seyda-neen-shore` reports its
  placed count of 8,535 on every run.
