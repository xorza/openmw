# Open issues

- `Surface::Material::mDiffuseColour` and `mEmissiveColour` reach `GpuMaterial` undecoded.
  `NifOsg` reads both off `NiMaterialProperty`, which stores them display-encoded, and
  `MaterialResolver::describe` copies them across without `decodeColour`. The shader multiplies the
  first into an albedo the sampler already brought to linear, and adds the second to linear
  radiance.
