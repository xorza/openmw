# Open issues

- `openmw-rtxtool verify`'s `island-crossing` view differs from itself run to run in one build
  (worst 4 of 255 on 0.01% of the pixels, release, upscaling off), while the other fifteen views
  are byte-identical between runs.

- `shot --accumulate N` darkens as `N` rises: the Balmora mages' guild reads a mean of 12.3 of 255 at
  eight frames and 2.7 at five hundred and twelve, at the same held exposure.
