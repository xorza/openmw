# Open issues

- `openmw-rtxtool doll` and `openmw-rtxtool map` write different bytes on two runs of one binary,
  with the same arguments. `shot` writes the same bytes twice.
- `openmw-rtxtool bench` reports a different `structureBytes` and `tableBytes` for an exterior place
  on two runs of one binary with the same arguments. Every other figure of that place agrees.
- A bounce ray leaves the shell of Balmora's Guild of Mages, so those points carry nothing from the
  escaped share where a wall would have handed back the room's fill. Measured over a converged
  1920x1080 frame of the wide view: 0.35% of the hemisphere on average, a 99th percentile of 10%,
  and 30 pixels of 2.07 million above half.
- A sprite in a room takes the fill through its own thickness alone. `puffLight` mixes to `fillLit`
  indoors and reads no answer from the world, so smoke under a table is as bright as smoke in the
  middle of the floor.
- `litCosine` weighs a light by the shading normal alone, so a normal past its own triangle's plane
  — 4% of hits carry one more than sixty degrees off — accepts light from behind the surface.
