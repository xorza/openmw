# Open issues

- `components-tests` does not compile in the `release` build with GCC 16.2:
  `apps/components_tests/rtx/slots.cpp` stops on `-Werror=stringop-overflow=` ("writing 8 bytes
  into a region of size between 1 and 5", reported inside `bits/stl_construct.h`). The `debug`
  build compiles it.
- A vertex attribute that is the same at all three corners does not come back exactly from its
  interpolation: `cornerWeights` in `lib/geometry.glsl` gives `1 - b.x - b.y`, `b.x` and `b.y`,
  whose float sum can be one less an ulp, so a vertex tint of 1.0 everywhere arrives as 0.99999994
  and one of 0.25 as 0.24999999. `RtxVisibilityTest.aSpecularMapReflectsTheLampByTheHostsLobe`
  holds the reflectance to an ulp for it.
