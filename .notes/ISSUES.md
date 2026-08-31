# Open issues

- `openmw-rtxtool doll` and `openmw-rtxtool map` write different bytes on two runs of one binary,
  with the same arguments. `shot` writes the same bytes twice.
- `openmw-rtxtool bench` reports a different `structureBytes` and `tableBytes` for an exterior place
  on two runs of one binary with the same arguments. Every other figure of that place agrees.
- An interior is not lightproof. `skyReaching` in Balmora's Guild of Mages has a mean of 0.91 and
  exceeds 0.5 on 90% of pixels, where a closed cell should read nought — so rays leave the interior
  and report open sky. Objects near a wall are lit by it.
- A bright band runs along the fold of the pillow and the sheet on the beds in Balmora's Guild of
  Mages, where a contact shadow would be, and it is brighter there than the sheet around it. It is
  the bounce and not the direct light: it survives with the painted light kept (`--delight=0`), with
  the emitters and people off, and with the lamp term removed entirely, and it goes when `pathEnd`
  returns nothing.
- `litCosine` weighs a light by the shading normal alone, so a normal past its own triangle's plane
  — 4% of hits carry one more than sixty degrees off — accepts light from behind the surface.
