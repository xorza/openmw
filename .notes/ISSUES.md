# Open issues

- `apps/components_tests/rtx/scenedesc.cpp:611` — `aFixedSpriteReachesByItsOwnAxesAndAnEyeFacingOneByItsRadius`
  binds `const SpriteEmitter& rain` into `SceneDesc`'s emitter vector, then calls
  `addEmitter` again at `:620`, which reallocates it. Line `:622` reads the freed
  reference. AddressSanitizer aborts the whole `*Rtx*` run there.
