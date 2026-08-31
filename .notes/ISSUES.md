# Open issues

- `ShadingMap`'s constructor asserts `sampled > 0` and three of the seventeen `verify` views break
  it: `addamasartus`, `balmora-mages-guild` and `vivec`. Some texture they use resolves into no
  cell at all, so `openmw-rtxtool shot --view=<any of the three>` aborts in any build whose asserts
  are compiled in.
