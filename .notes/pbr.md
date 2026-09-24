# PBR materials in the RTX renderer

Status: research and design. No code is changed yet.

## 1. Summary

- **One material model for all content.** The model is GGX metal/roughness in the glTF 2.0
  form. A vanilla surface is this model with F0 = 0. With F0 = 0 the specular term is exactly
  zero, and the diffuse term is exactly the Lambert term of today, on the same shader path.
- **The maps are found by name.** The renderer uses the same rule and the same `[Shaders]` keys
  as the rasterizer. One new `[RTX]` key states the `_spec` channel layout, because a file cannot
  state it, and real content has two layouts (section 2.3).
- **Vanilla pictures do not change.** Each phase in section 7 ends with
  `shot --views=all --against=<baseline>` on the vanilla profile, and that run must find no
  changed picture.

## 2. The content

### 2.1 The convention

The files sit beside the diffuse `foo.dds`. The rasterizer finds them by name. The NIF does not
name them.

| file | channels |
|---|---|
| `foo_nh.dds` | RGB: tangent-space normal. A: height (parallax). OpenMW looks for `_nh` first. |
| `foo_n.dds` | RGB: tangent-space normal. OpenMW looks for it when there is no `_nh`. |
| `foo_spec.dds` | PBR layout: R metalness, G perceptual roughness, B AO, A SSS (`sss = 1 - A`). |
| `foo_diffusespec.dds` | Terrain only. It replaces the diffuse. RGB diffuse, A roughness (Wareya). |

- Lookup rule: `components/shader/shadervisitor.cpp:333-424`. PBR reading: Wareya's
  `specMapToPBR` (`shaders/lib/light/lighting_pbr.glsl:902`).
- Roughness: Wareya squares G, so `alpha = G²`. Measured on the installed files, G is roughness:
  wood 0.92–1.00, stone 1.00, steel 0.36–0.44.
- F0: `mix(0.04, max(0.01, albedo), metal)`, F90 = 1. This is glTF with a floor.
- Normal maps: DirectX style (green is −Y), per
  `docs/source/reference/modding/texture-modding/texture-basics.rst:18`. The tangent comes from
  `osgUtil::TangentSpaceGenerator` on UV0. The binormal is `cross(N, T) * T.w`
  (`files/shaders/compatibility/normals.glsl`). Terrain uses the fixed tangent `(1, 0, 0, 1)`.
- Two-channel normal maps (BC5/ATI2, R8G8): Z is reconstructed from XY. A three-channel map uses
  its stored XYZ.
- Parallax: `uv + eye.xy * (height * 0.04 - 0.02)`, with `eye` the unit view direction in
  tangent space (`files/shaders/lib/material/parallax.glsl`).

### 2.2 The installed PBR mods

- Mod 57486 (Tamrielic armors) was made for Rafael's shaders at
  `SPECULAR_MAP_INTERPRETATION 2`. Its files read as the same layout: steel has R 0.87, cloth
  has R 0.
- The MVR PBR main file marks nothing as metal. R is 0 in all 3390 `_spec` maps, gold and steel
  included. The metals are in the optional "Wareya metal textures" file of 57592.
- The author says the 57592 metals were tuned against Wareya's compensations (for example
  `PBR_METAL_F90_DARKENING_HACK`), so their albedo is not a measured metal F0. The renderer does
  not copy those compensations. The fix for that is in the content (57486).
- 185 of the 3390 `_spec` maps carry an SSS alpha. The rest are BC1, so their A is 1 because of
  the format, not because of a choice.
- **Three MVR PBR diffuse maps are broken.** `TX_B_N_Argonian_M_N.dds`, `TX_B_N_Argonian_F_N.dds`
  and `tx_b_vivec_n.dds` are the vanilla neck diffuses (`_n` means "neck" there). The mod ships
  them as BC5, which has two channels. Their mean R and G are 0.58 and 0.38, so they hold colour
  and not normals, and blue is lost. The mod ships the real neck normal maps as `…_N_n.dds`. The
  author's tool probably compressed every `*_n.dds` as a normal map. The RTX renderer refuses the
  three (format 36285) and draws grey. These are the only BC5 files in the mods, so no real normal
  map is BC5. Phase 1 must keep BC5 refused in a colour slot.

### 2.3 A second `_spec` layout exists

M[FR] ships 14 `_spec` maps in OpenMW's classic layout: RGB is highlight colour and A is
glossiness. They are grey (R = G = B), for example `iron_ore_spec.dds` at 0.83 and
`elven_cuirass_f_tx_spec.dds` at 0.10. The M[FR] profile has `auto use object specular maps = true`.
Read as PBR, the iron ore becomes 83% metal and the cuirass becomes nearly a mirror.

So the auto-use switch says that the maps must be used. It does not say what their channels
mean. Rafael's shaders have the same problem, and they answer it with a user setting
(`SPECULAR_MAP_INTERPRETATION`). M[FR] also has 459 `_n` and 4 `_nh` maps, and those have one
meaning only.

## 3. Where the renderer stands

- **The maps never arrive.** `RtxRenderer::configureResources` calls
  `setShadersEnabled(false)`, so `ShaderVisitor` never attaches `normalMap` or `specularMap`.
  `Rtx::mapOf` drops the `Normal`, `NormalHeight` and `Specular` roles. `material.hpp` records
  this as a decision ("No normal map and no specular map, by decision").
- **Lambert only.** `shadeSurface` returns `albedo * (incoming + gather + emissive) + emitted`.
  `gather` returns direct light per unit albedo at `INV_PI`. `bounceLight` samples a cosine lobe.
  `lambertResponse` reports roughness 1 and specular albedo 0.
- **Vanilla has no specular by statement.** `nifloader.cpp:2891-2896` sets specular to black on
  every Morrowind NIF, because "Morrowind has its support disabled". `water.glsl:214` records the
  same.
- **Delight.** Every diffuse is divided by `paintedLight` at the run's `frame.mDelight`. PBR albedo
  is already delit.
- **Textures.** A slot is a file and its wrap (`TextureTable`). `SceneTextures::describe` opens
  each slot's file by path through `Resource::ImageManager` on the frame thread. Formats are BC1,
  BC2, BC3, RGBA8 and BGRA8, all sRGB. Every file slot gets a painted-light map
  (`ShadingPass`, 2 KiB).
- **Meshes.** Vertex streams are positions (position fetch), normals, UVs, second UVs and colours.
  There are no tangents. `skin.comp` poses positions and normals into the slot's copy. `morph.comp`
  moves positions only.
- **Material row.** `GpuMaterial` is 88 bytes. `MATERIAL_*` uses bits 0x01–0x10 and the dark-map
  unit in bits 8–11.
- **Guides and channels.** The trace writes `direct` (already multiplied by the albedo) and
  `indirect` (the diffuse bounce with the albedo divided out). `composite.comp` returns
  `direct + albedo * indirect`, and that colour goes to Ray Reconstruction. The payload already
  carries a specular albedo and a roughness. Only water fills them.
- **Threads.** The ring's `CellReader` reads models and ground textures on its own thread and
  lends them by path (`PreparedModel`, `PreparedTexture`). Objects in the active grid that the ring
  already read find their materials already adopted. Interiors, actors and items that appear in
  play arrive through the frame walk only.

## 4. Established practice

| topic | practice | source | here |
|---|---|---|---|
| Material model | Metal/roughness. `c_diff = mix(base, 0, metal)`, `F0 = mix(0.04, base, metal)`, `alpha = roughness²`, `f = (1 - F) c_diff / π + F D V`. | glTF 2.0, Appendix B | The model. |
| NDF, masking | GGX (Trowbridge-Reitz) with height-correlated Smith masking. | Walter 2007, Heitz 2014, Filament | The lobe. |
| Fresnel | Schlick with `F90 = saturate(50 · F0.g)`: no real material is under 2%, so less is specular occlusion. | Filament "Specular occlusion", UE4 `F_Schlick` | F0 = 0 gives exactly no specular. This is what keeps vanilla exact. |
| Diffuse | Lambert. Burley is not worth its cost. | Filament | Keep Lambert. |
| Energy loss | Single-scatter GGX loses energy as roughness grows. Scale the lobe by `1 + F0 (1/E(μ) - 1)`, with E the directional albedo at F0 = 1, from a table. | Kulla and Conty 2017, Turquin 2019, Filament | A 2D table, made once at startup by integration. |
| Roughness floor | Clamp perceptual roughness (0.045 in fp32 for analytic lights). | Filament, Frostbite | Point-like lamps and the sun disc. |
| Lobe sampling | Sample the visible normals (VNDF). The spherical-cap form is simpler and up to 39% faster. The bounded form wastes fewer rays below the horizon. | Heitz 2018, Dupuy and Benyoub 2023, Eto and Tokuyoshi 2023 | The specular bounce. |
| Area lights | A path tracer evaluates the BRDF at the point it sampled on the source. No representative-point fit is needed. | pbrt, standard Monte Carlo | Evaluate at the direction the shadow ray already samples. |
| Tangents | Per vertex. glTF names MikkTSpace for absent tangents. | glTF 2.0 | The content was checked against `osgUtil::TangentSpaceGenerator`, so match that. |
| Bad shading normals | Normal maps make back-facing and energy-losing normals. Microfacet normal mapping is the principled fix. Tilting the normal toward the view is the cheap one. | Schüssler et al. 2017, Tokuyoshi 2021 | The cheap one first. `water.glsl` already does it (`WATER_MIN_FACING`). |
| Specular aliasing | Widen roughness from the length of a mip-averaged normal, or from the pixel footprint. | Toksvig 2005, Kaplanyan et al. 2016, Tokuyoshi and Kaplanyan 2021 | Later, after a measurement of the mips (section 5.11). |
| AO map | Rasterizers apply it to indirect light only. A path tracer computes occlusion, and RTX Remix's material has no AO input. | glTF 2.0, Filament, dxvk-remix `rtx_materials.h` | Not read at first (decision 4). |
| RR guides | Roughness is linear (perceptual) roughness. Specular albedo is the pre-integrated specular (`EnvBRDFApprox2`, which also has the `saturate(50 · F0.g)` factor). Sky albedo 0.5. | DLSS-RR Integration Guide §3.4, Appendix | Fill both guides on every surface. |
| Denoise without RR | Separate demodulated diffuse and specular channels. Q2RTX filters specular in time only. | NRD, Q2RTX (see `denoiser-without-rr.md`) | A specular channel belongs to that work, not to this one (section 5.9). |

## 5. The design

### 5.1 One model, with vanilla inside it

Every surface carries a diffuse albedo, an F0 and a roughness. The values come from the maps where
the content has them, and from fixed values where it has none:

| content | diffuse albedo | F0 | roughness | normal | delight |
|---|---|---|---|---|---|
| vanilla texture | texture × tint | 0 | 1 | interpolated | on |
| `_n` or `_nh` only (M[FR]) | texture × tint | 0 | 1 | mapped | on |
| PBR `_spec` | `(1 - metal) × base` | `mix(0.04, base, metal)` | G | mapped if a map exists | off |

With F0 = 0 and `F90 = saturate(50 · F0.g)`, F is exactly 0. The diffuse factor `1 - F` is then
exactly 1, and the specular term is exactly 0. So a vanilla surface gets the same numbers from the
same code, and no branch is needed. This is also the content's own statement: Morrowind NIFs have
no specular.

A PBR `_spec` map means that the albedo was authored as an albedo. So such a material skips the
delight. A normal map alone does not mean that, so a material with only `_n` keeps the delight.

### 5.2 Switches

- `[Shaders] auto use object normal maps`, `auto use object specular maps`,
  `auto use terrain normal maps`, `auto use terrain specular maps` and the four patterns. These
  keys describe the content, and the launcher already shows them. The renderer reads them as the
  rasterizer does.
- New: `[RTX] specular map layout = ignore | metal roughness`. The default is `ignore`, because
  OpenMW's documented `_spec` layout is the classic one, and section 2.3 shows it in real content.
  The PBR profile sets `metal roughness`. The key needs the settings page and its translations.
- Harness: `--material-maps=false` turns the lookup off for an A/B run, like `--delight=0`.

### 5.3 Data flow

```
state set chain ──describeSurface──▶ SurfaceDescription   (+ Normal and Specular maps, when bound)
                                          │
diffuse file name ──MapFinder──▶ companion paths           (VFS lookup, once per diffuse path)
                                          │
        reader thread: open the images, lend them          walk: ask the reader, wait for them
                                          ▼
                         Material (+ mNormal, mSpecular, flags)
                                          │
        TextureTable (file, wrap, encoding) ──▶ SceneTextures ──▶ backend (UNORM views, no painted-light map)
                                          ▼
                         GpuMaterial (+ 2 words, 3 flags)          MeshTable (+ tangent word per vertex)
                                          ▼
          resolveFor: mapped normal, F0, roughness, side normal ──▶ gather: diffuse and specular apart
                                          ▼
          bounceLight: lobe choice, VNDF ──▶ Answer: specular in direct, guides ──▶ composite ──▶ RR
```

### 5.4 Maps: lookup, threads, slots

- **`SurfaceMap` gains `Normal` and `Specular`.** `mapOf` keeps `Normal` and `NormalHeight` as
  `Normal`, with a height flag on the description. A map that a state set binds (a NIF with a
  shader property) then arrives by the same path as a companion.
- **`MapFinder` (new, `components/rtx/`).** It takes the diffuse file name and answers the
  companion paths that exist in the VFS, with the rasterizer's rule: `_nh` before `_n`, then
  `_spec`. The switches and the patterns come in one conventions record. It keeps its answers in a
  flat table sorted by path, reserved once, so each diffuse path costs one lookup for the life of
  the run. It decodes nothing.
- **The companion images load off the frame thread.** An image decode is a file read of up to a
  few megabytes, and a frame must not pay for it. For ring cells, `CellReader` opens the companion
  images when it reads the model, and lends them as it lends ground textures. For a walk arrival
  (interiors, actors, new items), `MaterialResolver` sends the paths to the same reader thread. The
  material is adopted without the maps, and it is rewritten through `setMaterial` when the images
  arrive. The ground composite already works this way (`Material::mFlatten`). The surface shows
  vanilla shading for a few frames and then changes. That is a pop, and it is the price of
  uniform frames.
- **The resolver keeps the companion slots on the diffuse image's entry.** `HeldTexture` gains the
  normal and specular slots for each wrap. A companion takes the diffuse's wrap, as in
  `ShaderVisitor`. `Material::forEachTexture` names the two new maps, so the material table holds
  them while a material names them.
- **A slot is a file, its wrap and its encoding.** `TextureRow` gains `TextureEncoding { Colour,
  Data }`. A data slot gets UNORM formats: `Bc1RgbaUnorm`, `Bc3Unorm`, `Bc5Unorm`, and
  `Rgba8Unorm`, which exists already. A data slot gets the neutral painted-light map
  (`TextureData::hasNeutralShading`) and no mean texel.
- **Flags come from the image.** A two-channel normal map (BC5, RG8) sets a flag for Z
  reconstruction. `_nh` sets the height flag. A PBR `_spec` sets the authored-albedo flag. All
  three are known when the images are open, which is before the material takes the slots.

### 5.5 Tangents

- **Per vertex, with the `osgUtil::TangentSpaceGenerator` algorithm.** For each triangle it takes
  dP/du and dP/dv from the UVs, projects both onto each corner's normal plane, and adds them to
  the corner. At the end it normalizes, and it stores the handedness `w = sign((T × B) · N)`. The
  mod authors checked their normal maps against this code in OpenMW, so the renderer matches it,
  not MikkTSpace.
- **One word per vertex.** The tangent is octahedral in 2 × 15 bits, with the handedness in one
  bit. At the ship view that is about 2.6 MB (658 thousand vertices, from the vertex-colour bytes).
  Every mesh gets the stream, because it is small, and because a mesh does not know its material
  when it is read.
- **Made where the arrays are made.** `MeshReader` fills a persistent scratch per drawable, and
  `PreparedModel` carries the result for ring models. The work is a few operations per triangle,
  and it runs once at load.
- **Posed with the normals.** `skin.comp` takes the tangent through the same blended matrix and
  writes it beside the posed normal. `morph.comp` leaves it, as it leaves the normal. Worn armor is
  skinned, and the Tamrielic maps are armor, so this is not optional.
- **Rejected: a tangent per triangle from the posed positions.** It needs no storage and no skin
  work (pbrt uses it when a mesh has no tangents). But the tangent then jumps at each triangle
  edge, and Morrowind's meshes have few triangles, so the normal-mapped light shows facets.

### 5.6 The hit

`resolveFor` reads the new maps only where the material names them (`!= NO_TEXTURE`), as it does
for the dark map, the emissive map and the environment sheet. The branch is uniform over a mesh.

- **Normal.** Read the map at the same `TexturePoint` as the diffuse. Decode `2 * rgb - 1`, or
  reconstruct Z for a two-channel map. Build the frame from the interpolated tangent, the
  interpolated normal and `B = cross(N, T) * w`. Orthonormalize T against N first.
- **Facing.** If the mapped normal faces away from the ray, tilt it toward the interpolated normal
  until it faces the ray at a minimum cosine. Move that solved blend out of `shadeWater` into one
  shared function, so water and solids use one statement.
- **Side.** `Surface` gains `mSmooth`, the interpolated normal before the map. `gather` takes a
  light's side from `mSmooth` on a closed mesh and from the plane on an open mesh, as today. A
  normal map then cannot move a light to the other side of the surface.
- **Specular map.** R, G and B as section 2.1. The run's layout setting reaches the shader as the
  presence of a slot: with `ignore`, no material names a `_spec` slot.
- **`Surface` gains** `mSpecular` (F0, vec3), `mRoughness` (perceptual) and `mSmooth`. The
  environment sheet (`spherePoint`) takes the mapped normal, as `objects.frag` does.

### 5.7 Direct light

- `gather` returns two sums: `mDiffuse` (per unit albedo, with `1 - F` in it) and `mSpecular`
  (`F D V cos L`, with the energy compensation). `shadeSurface` becomes
  `albedo * (incoming + g.mDiffuse + emissive) + g.mSpecular + emitted`.
- **The lamp reservoir keeps its weight.** The weight stays the Lambert cosine per unit albedo.
  After the choice, the chosen lamp gets the full BRDF. The estimate is the chosen lamp's full term
  over the chance of its choice, so it stays unbiased. The target function changes only the
  variance. Vanilla keeps the same lamp choices and the same numbers.
- **The BRDF uses the direction that the shadow ray samples.** `lampVisible` and `skyVisible`
  already draw a direction across the lamp's sphere and the sun's disc. They return it, and the
  specular term is evaluated there. That is the Monte Carlo estimate of the area light, and it
  needs no representative-point fit.
- **No MIS is needed.** A bounce never counts the sun disc (`bounceEscape` returns the glow only),
  and lamps are not in the acceleration structure. So a lamp and the sun arrive by one estimator
  only. The roughness floor limits the variance of a sharp lobe on a small source.
- **Later, after a measurement:** add the specular term to the lamp weight, if shiny metal under
  many lamps is noisy.

### 5.8 The bounce

- One ray, as today. A draw from its own sequence (`SEED_BOUNCE_LOBE`, new) picks the lobe with the
  chance `p = lum(Es) / (lum(Es) + lum(c_diff))`, where `Es` is the specular albedo from the table.
  With F0 = 0, `p` is exactly 0, the draw decides nothing, and the diffuse direction draws are the
  same draws as today. That is the pattern of `SEED_SHEET_SIDE`.
- Diffuse lobe: the cosine sample of today, weighted by `(1 - F(v·h)) / (1 - p)`.
- Specular lobe: a VNDF sample (spherical caps) about the mapped normal. The weight is
  `F · G2 / G1 · compensation / p`. A direction behind the plane returns nothing (`behindTheFace`),
  as today.
- The cone opens with the lobe, not with `BOUNCE_SPREAD`. The spread comes from the roughness and
  must be measured.
- The bounce rate (`mBounceRate`) applies to both lobes at first. Measure if glossy reflections
  need every ray.

### 5.9 Guides and channels

- `SurfaceResponse` of a solid: the mapped normal, `c_diff`, the specular albedo `Es` (with the
  compensation) and the perceptual roughness. For vanilla that is `albedo`, 0 and 1, the same as
  `lambertResponse` today.
- `Es` comes from the table in section 4. A test holds it within the stated error of
  `EnvBRDFApprox2` from the RR guide.
- **The specular bounce goes into the direct channel.** Ray Reconstruction takes the composite
  colour, so it gets the same input with or without a separate channel. The payload stays at 17
  words, and no image is added. Without RR, the direct channel is not spatially filtered, and that
  is already true of all direct light (`denoiser-without-rr.md`). The demodulated specular channel
  belongs to that denoiser work.
- Later: the RR specular hit distance, for sharper glossy reflections.

### 5.10 Terrain

- `GpuLayer` gains a normal slot and a flags word. The tangent is the fixed `(1, 0, 0)` in chunk
  space, as in `terrain.vert`. Layer normals are summed by their weights and normalized.
- `_diffusespec` replaces the layer's diffuse, as in OpenMW. Its A is roughness under the
  `metal roughness` layout. Metal is 0 for ground.
- A flattened distant chunk (`groundcomposite.comp`) keeps the geometric normal and roughness 1.
  Its texels are wider than any normal-map detail.
- Terrain comes after objects, because each layer adds fetches, and that cost must be measured
  alone.

### 5.11 What is left out, and why

- **AO (B).** Not read at first. The traced bounce and `ambientReaching` already find the
  occlusion of real geometry. The map would count that occlusion twice. An A/B with AO on the
  indirect term only can follow.
- **SSS (A).** Not read at first. The 3204 BC1 maps cannot carry it, so A = 1 there means
  nothing. The existing sheet transmission (`SHEET_TRANSMISSION`) stays the model for leaves.
- **Parallax (`_nh` A).** Phase 6. OpenMW's form is one extra fetch and one offset.
- **Specular aliasing.** First measure whether the mips of the installed normal maps keep the
  averaged length (texconv can renormalize each mip). Toksvig works only on averaged mips.
- **Classic `_spec` maps.** Declined. No physical mapping from a highlight colour to F0 exists.

### 5.12 Cost

- **Hit.** For a material with maps: two fetches, one tangent fetch, and the frame. For a vanilla
  material: two compares. The GGX terms in `gather` run on every hit with F0 = 0. That is a factor
  of zero, as AGENTS.md prefers. `bench` must show no change of the `trace` zone on vanilla, or the
  terms get a uniform branch on F0.
- **`Surface`** grows by 7 floats. Check the stage for spills, as the `SkyChoice` comment did.
- **Memory.** 4 bytes per vertex, 8 bytes per material. PBR maps take about three times the
  texture memory and slots of the diffuse maps. The ship view uses 606 of 4096 slots and 860 MiB
  for diffuse maps only. The budget and the slot limit refuse what does not fit, and `scene`
  reports it.

## 6. Decisions

Items 1–3 are decided. Items 4–6 are recommendations, and each is settled in the phase that
reaches it.

1. **Decided: reverse the recorded decision.** `material.hpp`, `surface.hpp` and the AGENTS.md
   posture ("Vanilla content, new light transport") say that the renderer does not rank mod
   compatibility. Phase 1 reverses that, and "vanilla pictures do not change" is the new rule.
2. **Decided: `[RTX] specular map layout` defaults to `ignore`**, for section 2.3.
3. **Decided: vanilla F0 is 0.** It is the content's statement, it is exact, and it is free of a
   guess. The standard dielectric (0.04 at roughness 1) would change every vanilla picture.
4. **AO.** Recommendation: not read, then an A/B.
5. **Walk arrivals.** Recommendation: load the companions on the reader thread and accept the pop.
   The alternative is a decode on the frame thread and a spike when an actor equips armor.
6. **Specular bounce channel.** Recommendation: the direct channel now, a separate channel with
   the non-RR denoiser.

## 7. Implementation plan

Each phase ends with the verification chain of AGENTS.md: build, the covering tests with a
filter, `rtx debug test`, `shot --views=all --against=<vanilla baseline>` with no changed picture,
and `rtx debug repeat --pairs=10` where a frame reads the change. Phases 3 and 4 also run `bench`
on both profiles. `rtx debug gate` runs once at the end of each phase.

**Phase 0 — baseline.** Record the vanilla baseline pictures (`shot --views=all`) and a vanilla
`bench`. Record the PBR profile `scene` census. Answer decisions 1–3.

Done at 711f4b27c6 (release). Everything is in `build-release/pbr-baseline/`:
- `vanilla/` and `pbr/`: `shot --views=all --map` of the two profiles, 23 views each. The later
  checks are `shot --views=all --map --against=build-release/pbr-baseline/vanilla` and the same
  with `--replace=config --config=$HOME/.config/openmw-pbr --against=…/pbr`. A second vanilla
  run against the first found all 46 pictures and all 184 frame hashes the same.
- `census.txt` and `census/`: one `scene` process per view and profile. The PBR diffuse maps
  alone take 3 to 10 times the vanilla texture memory: 1085 against 149 MiB at `balmora`, 860
  against 116 at the ship. The largest slot count is 737 at `balmora`, far under 4096.
- `bench/`: the 2026-09-24 entry of `.notes/bench.txt` has the figures. The PBR diffuse maps
  alone cost the trace 0.06–0.10 ms at the ship (5.21–5.31 against 5.31–5.37 ms), and nothing
  measurable in the guild or at Balmora. Phases 3 and 4 compare against these legs.
- The PBR profile refuses 1 or 2 textures in six views, and vanilla refuses none. Section 2.2
  says why.

**Phase 1 — maps reach the scene (no picture change).**
- `surface.hpp/.cpp`: `SurfaceMap::Normal` and `Specular`, `mapOf`, the height flag.
- `mapfinder.hpp/.cpp` (new): the conventions record and the sorted table.
- `texturetable.hpp/.cpp`: `TextureEncoding` in the row and the key.
- `texturedata.hpp`, `texels.cpp`, `texturebuilder.cpp`: UNORM formats, `readFormat` by encoding,
  neutral painted light for data.
- `rtxvulkan/formats.cpp`, `texture.cpp`: the Vulkan formats, and no `ShadingPass` for data slots.
- `material.hpp`: `mNormal`, `mSpecular`, three flags, `forEachTexture`. Rewrite the "by decision"
  comment. `materialresolver.hpp/.cpp`: companion slots on `HeldTexture`.
- `cellreader`, `prepared.hpp`, `cellsupply`: lend companion images, and take path requests from
  the walk. `materialresolver`: the pending list and the rewrite on arrival.
- `apps/openmw/mwrender/rtx/rtxsettings`, `worldmirror`, `components/settings/categories/rtx.hpp`,
  `files/settings-default.cfg`, the settings layout and its translations: the layout key and the
  conventions record. `apps/rtxtool/options.cpp`: `--material-maps`.
- `scene` report: companion maps found, by role and format, and their bytes.
- Tests: `MapFinder` (the `_nh` order, the switches off, a missing file), the texture key by
  encoding, the formats, the hold and drop of the new slots (the extractor `materials` test), the
  pending rewrite.
- Exit: the PBR `scene` shows the maps, nothing refused. The vanilla pictures do not change,
  because no shader reads the new rows yet.

**Phase 2 — tangents (no picture change).**
- `tangentspace.hpp/.cpp` (new): the `osgUtil` algorithm over scratch buffers, and the packing.
- `meshreader`, `prepared.hpp`, `meshtable`, `scene.h` (a tangent block like `NormalBlock`),
  `scenebuffers`: the stream. `skin.comp`, `skinpass`, `skintables`: posed tangents.
- Tests: compare with `osgUtil::TangentSpaceGenerator` on the same geometry (a mirrored seam, a
  degenerate UV, a quad), the pack round trip, and the posed tangent against the host pose (the
  `skinning` extractor test).

**Phase 3 — the model in direct light and in the hit.**
- `shaders/brdf.h` (new, shared by host and device like `look.h`): GGX D, height-correlated V,
  Schlick F with F90, compensation, VNDF sampling. `look.h`: the dielectric F0, the roughness
  floor, the F90 factor.
- The specular-albedo table: made once at startup by integration, a baked slot or a table buffer.
- `scene.h`: `GpuMaterial` to 96 bytes, the flags. `scenebuffers.cpp` `toGpu`.
- `geometry.glsl` (tangent fetch), `texturing.glsl` (normal and specular reads, delight by flag),
  `traversal.glsl` (`Surface` fields, the frame, facing), `lights.glsl` (the sampled direction),
  `shading.glsl` (`gather` in two sums, `shadeSurface`, the response), `water.glsl` (the shared
  facing function).
- Debug views: `--show=normal|roughness|metal|specular` beside `mShowAlbedo`.
- Tests: host tests of `brdf.h` (the furnace: compensated white metal returns 1 within the table's
  error, reciprocity, F0 = 0 gives exactly 0, the table against `EnvBRDFApprox2`), and a device
  test that the shader's BRDF matches the host's.
- Exit: vanilla pictures do not change. The PBR views show armor highlights under the sun and
  lamps.

**Phase 4 — the specular bounce.**
- `shading.glsl` `bounceLight`: the lobe draw, the VNDF sample, the weights, the cone. `SEED_BOUNCE_LOBE`.
- Tests: the sampler's pdf against a histogram on the host, and the weight for F0 = 0.
- Exit: vanilla pictures do not change. `bench` on the PBR profile reports the cost, with p99 and
  the worst frame.

**Phase 5 — terrain.** Section 5.10. Tests: the layer normal sum, and `_diffusespec` replacing
the diffuse.

**Phase 6 — parallax on `_nh`.** OpenMW's offset, before all reads of the hit. Primary rays first.

**Phase 7 — measured follow-ups.** The lamp target with specular, specular aliasing, the AO A/B,
the SSS A/B, the RR specular hit distance.

**Documentation.** `docs/rtx/architecture.md` §7.7 and AGENTS.md change with phase 1 (decision 1).
`material.hpp` and `surface.hpp` lose the "by decision" text in the same phase.

## 8. Test profile

`~/.config/openmw-pbr/` is a copy of the vanilla `openmw.cfg` and `settings.cfg`. The data
folders are, in load order: `Morrowind-PBR/Data Files`, then `mods/1-mvr` (MVR Medium-HD 0.96a),
`mods/2-mvr-pbr` (MVR PBR v2 Beta, with `MetalMipmapsHotfix.zip` applied over it) and
`mods/3-tamrielic-pbr-armors`. Run with `--replace=config --config ~/.config/openmw-pbr`. The Zed
task "RTX: seyda-neen-ship, PBR mods (release)" does this.

- The optional "Wareya metal textures" file of 57592 goes between `2-mvr-pbr` and
  `3-tamrielic-pbr-armors`.
- Formats of the maps (DDS headers):

  | role | 2-mvr-pbr | 3-tamrielic |
  |---|---|---|
  | `_n` | BC1 2639, BC3 2, BC5 3 | BC1 109, BC3 2 |
  | `_nh` | BC3 1185 | BC3 4 |
  | `_spec` | BC1 3204, BC3 186 | BC1 114, BC3 1 |
  | `_diffusespec` | BC3 165 | — |

  No BC7 anywhere. The three BC5 `_n` files are the broken neck diffuses of section 2.2, not
  normal maps. BC5 stays in phase 1 because OpenMW documents two-channel normal maps, and other
  content can ship them.
- Ship view (`scene --view=seyda-neen-ship`): 606 textures in 860 MiB, against 116 MiB for
  vanilla, before any `_n` or `_spec` map is read. None refused.
- GL reference: `resources-wareya/` beside `mods/` links to `build-release/resources` and has
  its own `shaders/` with Wareya's files over it. Set `[RTX] enabled = false` in the profile and
  add `--resources /home/Games/Morrowind-PBR/resources-wareya`.

## Sources

- glTF 2.0 specification, Appendix B: https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html
- Filament, "Physically Based Rendering in Filament" (energy compensation, specular occlusion,
  roughness clamp, AO): https://google.github.io/filament/Filament.html
- DLSS-RR Integration Guide, §3.4 and Appendix (`/home/xxorza/Projects/dlss/doc`).
- Heitz, "Sampling the GGX Distribution of Visible Normals", JCGT 7(4), 2018.
- Dupuy and Benyoub, "Sampling Visible GGX Normals with Spherical Caps", CGF 42(8), 2023:
  https://arxiv.org/abs/2306.05044
- Eto and Tokuyoshi, "Bounded VNDF Sampling for Smith–GGX Reflections", SIGGRAPH Asia 2023 TC:
  https://dl.acm.org/doi/10.1145/3610543.3626163
- Kulla and Conty, "Revisiting Physically Based Shading at Imageworks", SIGGRAPH 2017 course.
- Turquin, "Practical multiple scattering compensation for microfacet models", 2019.
- Schüssler, Heitz, Hanika, Dachsbacher, "Microfacet-based Normal Mapping for Robust Monte Carlo
  Path Tracing", SIGGRAPH Asia 2017: https://dl.acm.org/doi/10.1145/3130800.3130806
- Tokuyoshi, "Unbiased VNDF Sampling for Backfacing Shading Normals", SIGGRAPH 2021 talk.
- Toksvig, "Mipmapping Normal Maps", JGT 2005. Kaplanyan et al., "Filtering Distributions of
  Normals for Shading Antialiasing", HPG 2016.
- dxvk-remix `src/dxvk/rtx_render/rtx_materials.h` (the opaque material's inputs).
- OpenSceneGraph 3.6.5 `src/osgUtil/TangentSpaceGenerator.cpp`.
- wareya/OpenMW-PBR: https://github.com/wareya/OpenMW-PBR
