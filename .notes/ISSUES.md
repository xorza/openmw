# Open issues

- `rtx <flavour> kernels` (`kernelDigest` in `apps/rtxtool/rtx`) freezes the specialization
  constants but does not fold the spec-constant operations built on them, so a branch on
  `!HAS_MAPS` or another `OpSpecConstantOp` stays in the digest. The `HAS_MAPS=0` tuple of
  `visibilityhit.rchit` digests 35364 lines of disassembly where the folded module has 10832, the
  whole specular half included, and a change to code that no vanilla tuple runs moves every
  vanilla digest.
- The same function's `awk` pattern `/^ *OpFunction /` never matches, because `spirv-dis --raw-id`
  writes a function as `%id = OpFunction ...`. Every instruction of every function body is
  therefore sorted with the declarations, and the digest does not hold the order of the code.
- The specular albedo table (`Rtx::SpecularAlbedo`, `specularAlbedoAt`) puts its cells' centres at
  `(i + 0.5) / 32` and holds the outer ones past them, so a surface seen square on is compensated
  off a cosine of 0.984. At a roughness of 128/255 the lobe's whole is 0.91442 at one and the table
  gives 0.91286 there, and a white metal square on gives back 1.0017 of an even sky — measured by
  `aGlossyFloorUnderAnEvenSkyGivesBackWhatItReflects`. Toward grazing the first centre is 0.016.
