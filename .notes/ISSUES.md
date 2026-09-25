# Open issues

- The frames of one build differ between processes. The NVIDIA driver (615.71.09) keeps per-branch
  counters of each ray-tracing shader in its disk cache and compiles the launches a second time from
  them, and the second code rounds differently from the first. Both codes decoded from one cache
  show the same `OpDot` in a fog raygen summed z, y, x in the first and x, y, z in the second, and a
  different count of fused multiply-adds (405 against 407 in one part). The Vulkan spec lets
  `OpDot`, the matrix products, `length`, `normalize`, `cross`, `mix`, `reflect`, `refract` and
  `faceforward` take any order, and lets any float add or multiply that is not `NoContraction` be
  fused or reassociated. A process can start on either code or change partway: of 26 `one-cell-walk`
  benches (14 release, 12 debug), 6 differed from the rest in g-depth, g-motion, g-direct and
  composite. `rtx debug gate`'s repeat pair failed on it twice.
