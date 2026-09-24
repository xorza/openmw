# PBR materials in the RTX renderer

## The content convention

The files sit beside the diffuse `foo.dds`. The rasterizer finds them by name, not through the NIF.

| file | channels |
|---|---|
| `foo_nh.dds` | RGB: tangent-space normal. A: height (parallax). OpenMW looks for `_nh` first. |
| `foo_n.dds` | RGB: tangent-space normal. OpenMW looks for it when there is no `_nh`. |
| `foo_spec.dds` | R: metalness. G: perceptual roughness. B: AO. A: SSS, where `sss = 1 - A`. |
| `foo_diffusespec.dds` | Terrain only. Diffuse RGB. A: roughness in Wareya's `terrain.frag`. |

- Source: `components/shader/shadervisitor.cpp:333-424`, and Wareya's
  `specMapToPBR` (`shaders/lib/light/lighting_pbr.glsl:902` in github.com/wareya/OpenMW-PBR).
- Roughness: Wareya squares G, so `alpha = G²`. This is the Disney and glTF convention.
  Nexus describes mod 57592 as "smoothness" maps, but the author says G is roughness.
  Check one polished metal texture: roughness gives it a low G.
- F0: `mix(0.04, max(0.01, albedo), metal)`, F90 = 1. This is the glTF metal/rough model
  with a floor.
- Wareya's shader adds compensations, for example `PBR_METAL_F90_DARKENING_HACK`. The author
  says the metals in 57592 were tuned against them, so their albedo is not a measured metal F0.
  Mod 57486 has correct F0 values.
- Normal maps: DirectX style (green is −Y), per `docs/source/reference/modding/texture-modding/texture-basics.rst:18`.
  The tangent comes from `osgUtil::TangentSpaceGenerator` on UV0. The binormal is
  `cross(N, T) * T.w` (`files/shaders/compatibility/normals.glsl`). Terrain uses the fixed
  tangent `(1, 0, 0, 1)`.
- Two-channel normal maps (BC5/ATI2, R8G8): Z is reconstructed from XY.

## Where the renderer stands

- `RtxRenderer::configureResources` turns shaders off. `ShaderVisitor` never runs, so no
  state set gets a `normalMap` or a `specularMap`.
- `Rtx::mapOf` drops `Normal`, `NormalHeight` and `Specular`. `material.hpp` records this as a
  decision: "No normal map and no specular map, by decision".
- Shading is Lambert only: `lambertResponse`, `shadeSurface`, `gather` at `INV_PI`, and a
  cosine-sampled `bounceLight`.
- Every diffuse is delit by `paintedLight` at `frame.mDelight`. PBR albedo is already delit.
- Formats: BC1, BC2, BC3, RGBA8 and BGRA8, all read as sRGB. There is no BC4, BC5 or BC7, and
  no UNORM view for data maps.
- The Ray Reconstruction guides already carry specular albedo and roughness. Water fills them.

## Plan

Each step can be verified alone. The vanilla profile keeps the auto-use switches off, so
`shot --views=all --against=<baseline>` must show no change after each step.

1. **Find the maps at load.** Use the same rule as `ShaderVisitor`, the same `[Shaders]` keys
   (`auto use object normal maps`, `auto use object specular maps`, the three patterns), and
   one VFS lookup per diffuse path, kept in a table. Do not run `ShaderVisitor`, because it
   compiles GL programs and edits the geometry.
2. **Texture path.** A slot is a file, its wrap and its encoding: sRGB for colour and UNORM for
   data. Add BC4, BC5 and BC7 if the content uses them. `scene` lists the formats and the
   refusals. Data slots get no shading map and no mean texel.
3. **Material row.** Add a normal index and a specular index. Add flags: authored albedo
   (no delight), two-channel normal, and normal with height.
4. **Tangents.** Per vertex, with the same algorithm as `osgUtil::TangentSpaceGenerator` on UV0,
   packed in one word (octahedral and handedness). Only meshes whose material has a normal map
   get them. A host test compares them with the `osgUtil` output on the same geometry.
5. **Shading.** Use GGX metal/rough in the glTF convention: `F0 = mix(0.04, albedo, metal)`.
   Use `F90 = saturate(50 * luminance(F0))` (UE4 `F_Schlick`). Vanilla gets `F0 = 0`, so its
   specular is zero and its diffuse is exactly Lambert on the same path.
   - Direct light: evaluate the full BRDF for the sun and lamp direction that is chosen.
     The lamp reservoir keeps its diffuse target at first. It stays unbiased.
   - Bounce: one ray. A draw from its own seed picks the lobe by estimated weight. GGX uses
     visible-normal sampling (Heitz 2018, or Dupuy and Benyoub 2023). A vanilla hit always
     picks diffuse, so its sequence does not change.
   - RR guides: diffuse albedo = `(1 - metal) * albedo`. Specular albedo = the pre-integrated
     specular (a DFG table or Karis's analytic fit). Check the roughness convention in the
     DLSS-RR integration guide before writing the value.
   - The à-trous path divides the bounce by the diffuse albedo. A specular bounce must not go
     through that division (see `denoiser-without-rr.md`).
6. **AO.** The RTX Remix opaque material has no AO input (`rtx_materials.h` in dxvk-remix). The
   traced occlusion replaces it. Do not read B at first. An A/B with AO on the indirect term
   only can come later.
7. **SSS.** Use A to scale the existing sheet transmission on foliage. SSS on closed meshes
   (skin) is out of scope.
8. **Parallax.** The rasterizer uses a simple offset (`getParallaxOffset`). Offset the UV on the
   primary ray only. Do this later.
9. **Terrain.** Per-layer normal maps with the fixed tangent, and `_diffusespec` alpha as
   roughness. Do this later, because each layer adds fetches.
10. **Specular aliasing.** Mips shorten averaged normals. Widen the roughness from that length
    (Toksvig). Do this later.

## Decisions

- Reverse the recorded decision in `material.hpp` and `surface.hpp`, and the "Vanilla content"
  posture in `AGENTS.md`?
- Vanilla: exact Lambert (`F0 = 0`), or the standard dielectric `F0 = 0.04` at roughness 1?
  The second is physically standard and changes every vanilla frame.
- Delight: skip it only for materials with a `_spec` map, or also for materials with only a
  normal map?
- AO: ignore it, or apply it to the indirect term only?

## Test profile

`~/.config/openmw-pbr/` holds a copy of `openmw.cfg` and `settings.cfg`. Run both programs with
`--replace=config --config ~/.config/openmw-pbr`. Tested with `openmw-rtxtool scene`.
