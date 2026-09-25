# PBR materials in the RTX renderer

Status: phases 0 to 6 are built; phase 7, the measured follow-ups, is next.

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
  The PBR profile sets `metal roughness`. It is in `settings-default.cfg` and `rtx.rst`, and not yet
  in the launcher.

### 5.3 Data flow

```
model load (OpenMW's loading threads) ──Shader::AutoMapVisitor──▶ state set + normalMap / specularMap units
                                          │
state set chain ──describeSurface──▶ SurfaceDescription (+ Normal, Specular)
                                          │
MaterialResolver::describe ──▶ Material (+ mNormal, mSpecular; the specular map only under metal roughness)
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

Built in phase 1. This replaces the `MapFinder` and the async reader requests of the first design.

- **The maps are attached at model load, where OpenMW attaches them.** `ShaderVisitor` already
  looks up `_nh`, `_n` and `_spec` beside each diffuse map (`shadervisitor.cpp:333-424` before
  phase 1) and binds them with a `SceneUtil::TextureType`. That step moved to
  `components/shader/automaps.{hpp,cpp}` (`attachAutoMaps`), and `ShaderVisitor` calls it with no
  change of behaviour. When shaders are off, as under RTX, `SceneManager` runs
  `Shader::MapVisitor`, which does that step and the tangents of section 5.5, and nothing else. So the maps load on the threads
  that load the models (the cell preloader, and the ring's reader through
  `ContentSource::getTemplate`), and the frame walk, the ring and the doll views all find them in
  the state sets. There is no new thread, no pending rewrite and no pop. A model loaded on the frame
  thread pays for its maps as it pays for its diffuse maps, which is also true in GL.
- **Why not the whole `ShaderVisitor`.** It also moves the alpha test and the blend into removed
  state, builds GL programs and adds tangent arrays. The RTX renderer reads a model's state as the
  loader left it.
- **One switch record for both renderers.** `Shader::AutoMapRules` replaces five `SceneManager`
  setters, and `Renderer::prepareResources` fills it from `[Shaders]` for both renderers.
  `RtxRenderer::configureResources` then clears the specular switch under `ignore`, so a map that
  nothing reads is not loaded.
- **`SurfaceMap` gains `Normal` and `Specular`.** `mapOf` keeps `Normal` and `NormalHeight` as
  `Normal`, and `Specular` as `Specular`. A NIF that binds its own maps arrives the same way.
- **The layout reaches every resolver.** `MirrorKnobs::mSpecularLayout` comes from `RtxSettings`,
  `WorldMirror` hands it to its extractor, and `ViewRequest::mSpecularLayout` hands it to each
  picture inside the interface. The core reads no settings.
- **A slot is a file, its wrap and its encoding.** `TextureRow` has a `TextureEncoding { Colour,
  Data }`, and `HeldTexture` keeps a slot for each encoding and wrap. A data slot uploads as
  `Bc1RgbaUnorm`, `Bc2Unorm`, `Bc3Unorm`, `Bc5Unorm`, `Rgba8Unorm` or `Bgra8Unorm`, and takes the
  neutral painted-light map (`TextureData::hasNeutralShading`). BC5 is refused as a colour.
- **The scene digest names the new fields only where they are set**, and a data encoding only
  where it is data. So a vanilla scene digests as it did before phase 1, and `shot --against`
  still reports "the same" for vanilla.
- **Flags come later.** The two-channel, height and authored-albedo flags have no reader until
  phase 3, so phase 1 does not store them.

### 5.5 Tangents

Built in phase 2. This replaces the host copy of the `osgUtil` algorithm of the first design.

- **`osgUtil::TangentSpaceGenerator` itself, at model load.** `Shader::MapVisitor` carries the unit
  of the normal map in force down the graph, as `ShaderVisitor` carries its requirements. At each
  drawable under a normal map it runs the generator on the coordinates the map reads (its own
  unit's, else unit 0's, else the first array), and puts the result at texture unit 7
  (`Shader::sTangentUnit`), which is where `ShaderVisitor::adjustGeometry` puts it. A skinned or
  morphed drawable gets them on its source geometry, which is then set again. So both renderers
  use the same code, the same coordinates and the same drawables, and the mod authors checked
  their maps against that code. The generator stores `w = sign((T × B) · N)`, and the GL shader
  takes the bitangent as `cross(N, T) * w`.
- **Only under a normal map.** The visitor knows the state set in force, so a mesh no normal map
  is read through gets no tangents, as in GL. The first design gave every mesh tangents because
  the reader did not know the material.
- **One word per vertex.** `MeshReader` reads the `Vec4Array` at unit 7 into
  `MeshArrays::mTangents` (and no longer takes unit 7 for a second UV set), `PreparedModel`
  carries it for ring models, and `MeshTable` packs it (`tangent.hpp`): the direction octahedral
  in 2 × 15 bits with an odd step count, so that 0 and ±1 are exact, one bit for the handedness,
  and one bit for "present". 0 is no tangent, which every vanilla vertex has. Every vertex has a
  word, so the stream stays parallel to the others.
- **Posed with the normals.** `SceneBuffers` keeps a copy of the words per frame slot beside the
  normals (`mTangentTable`), and `SkinTables` keeps the bind words. `skin.comp` unpacks, applies
  the linear part of the blend, and packs again with the handedness kept, and 0 stays 0.
  `morph.comp` leaves the words, as it leaves the normals. The hit does not read them before
  phase 3, which adds the block table's address to the frame beside `mNormalBlocks`.
- **The scene digest** adds the words to the normals part only where a word is not 0, so a vanilla
  scene digests as the baselines on record.
- **Rejected: a tangent per triangle from the posed positions.** It needs no storage and no skin
  work (pbrt uses it when a mesh has no tangents). But the tangent then jumps at each triangle
  edge, and Morrowind's meshes have few triangles, so the normal-mapped light shows facets.

### 5.6 The hit

Built in phase 3.

- **The tangent** is fetched in `committedHit`, where the object-to-world matrix still is, and only
  from a mesh whose row says it has tangents (`MESH_TANGENTS`, from `MeshRange::mTangents`, which
  is set where any packed word is not nought). `Hit::mTangent` carries it in world space.
- **Normal.** Read the map at the diffuse's `TexturePoint`, decode `2 * rgb - 1`, and build the
  frame as `normals.glsl` does: the tangent unit, `B = cross(N, T) * w`, the interpolated normal,
  and **no orthogonalisation**, which is what the maps were checked against (the first design said
  to orthonormalise). Built on the normal as the mesh states it and then turned with it, so a sheet
  seen from behind sees the relief from behind. **Two-channel maps are found by their blue**: BC5 and
  RG formats read nought there and no three-channel map does, so `sampleNormalMap` rebuilds Z where
  blue is nought and needs no flag.
- **Facing.** A mapped normal that faces the ray less than `MAPPED_MIN_FACING` (0.03) is tilted
  toward the interpolated normal: `facingRay` in `basis.glsl`, which the water now calls too.
- **Side.** `Surface::mSmooth` is the interpolated normal, turned. `gather` takes a closed shape's
  light side from it.
- **Specular map.** Metalness in R, perceptual roughness in G. **A material with one is not delit**,
  splits `base * tint` into `mAlbedo = (1 - metal) base tint` and `mSpecular = mix(0.04, base, metal)
  * tint`, and the dark map multiplies both. **The tint and the dark map reach the lobe** because
  on this content they are light baked in, not colour: Wareya's `PBR_VERTEX_COLOR_HACK` (on by
  default) puts the vertex colour on the light, "otherwise darkened areas get a weird haze". That
  haze is what phase 3 drew first: the census office tapestry, which its vertices darken, reflected
  two to four times what it scattered, measured per pixel. With the tint on F0 it is 0.46 at the
  cloth and 0.10 at the wall. On F0, the tint also darkens the edge below 2%, which is the specular
  occlusion `saturate(50 F0.g)` is for.
- The environment sheet (`spherePoint`) takes the mapped normal, as `objects.frag` does.

### 5.7 Direct light

Built in phase 3.

- `gather` returns two sums (`DirectLight`): the diffuse half per unit albedo, with `F · diffuse`
  of each light taken off it (glTF's `1 - F`), and the specular half, `F D V (n.l)` with the energy
  compensation. `shadeSurface` is `albedo * (incoming + diffuse + emissive) + emitted + specular`.
- **The lamp reservoir keeps its weight**, the Lambert cosine per unit albedo, and the held lamp
  gets the lobe as well; the estimate divides by the same chances, so it stays unbiased.
- **The lobe is taken toward the source's centre, not where the shadow ray went** (a change from
  the first design). The centre is where the diffuse cosine and the reservoir's weight are taken,
  and a lobe taken elsewhere is divided by a weight that does not describe it: a lamp whose centre
  stands at the horizon weighs next to nothing while a point on its sphere may stand above it, and
  that quotient has no bound. The shadow ray still draws across the source, for the penumbra.
- **No MIS is needed**, for the first design's reason: no bounce counts the sun disc or a lamp.
- **Measured against the host**: `aSpecularMapReflectsTheLampByTheHostsLobe` holds the device to
  `brdf.h` and the table to a part in ten thousand for a metal, a dielectric, a tilted normal map and
  a tinted dielectric.

### 5.8 The bounce

Built in phase 4.

- One ray, as before. **A Lambert surface draws nothing new**: its bounce is the cosine sample of
  before, and the vanilla shots are the same frame for frame.
- A glossy surface draws which half from its own sequence (`SEED_BOUNCE_LOBE`), with the chance
  `p = lum(Es) / (lum(Es) + lum(c_diff))`, where `Es` is the compensated specular albedo from the
  table. Only off a sheet's near face: the far face transmits, and `gather` gives a light behind a
  sheet no lobe either.
- Diffuse half: the cosine sample, weighted by `(1 - F(v·h)) / (1 - p)`, into the indirect channel.
- Specular half: a VNDF sample about the mapped normal, by the spherical caps (`visibleNormal` in
  `brdf.h`, which the table now integrates by too). The weight is `F · G2 / G1 · compensation / p`
  (`smithShadowingGivenMasking`), into the direct channel (section 5.9). A reflection below the
  shading normal's horizon or behind the plane brings nothing back and is not traced.
- **Changed from the plan: the lobe's far hit is shaded as seen (`PATH_SEEN`), and its ray draws
  the faces the picture shows**, as the water's reflection does. A reflection is what the pixel
  shows, and at `PATH_INDIRECT` it lost the moons.
- **One path through the shader**: both directions are drawn and the chance selects one, and one
  `shadeAtPathEnd` call takes the path chosen at run time. Two calls, one per path, were two copies
  of the whole far hit, and a warp whose lanes drew both halves ran both. The path chosen at run
  time made `gather` divide a lone sun's weight by itself, which a device need not round to one,
  so `gather` now takes a source that holds all the weight whole.
- The cone: the pixel's spread and the lobe's width at half its peak, `ggxConeWidth`,
  `4 atan(α sqrt((√2 - 1) / (1 - √2 α²)))`, derived rather than measured. Held at `BOUNCE_SPREAD`,
  which it reaches at a roughness of 0.6. RTXPT widens a cone by `sqrt(1 / pdf)` of each sample, a
  heuristic by its own comment; a width per lobe is what the water and the diffuse bounce use.
- The escape is `skyGlow` for both halves: `gather` takes the lobe at the sun's centre, so the
  disc would count the sun twice.
- The bounce rate (`mBounceRate`) applies to both halves.

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

- `GpuLayer` gains a normal slot and a flags word, and is padded to 64 bytes. At 56 bytes, the
  two `vec4` of every second row are on 8 and not on 16. Then every layer that a hit sums, vanilla
  ground included, is read in 8-byte loads.
- The tangent is the fixed `(1, 0, 0)` in chunk space, as in `terrain.vert`, and the bitangent is
  `cross(N, x)`. The tangent-space normals of the layers are summed by their weights and carried
  once. A layer with no map adds `(0, 0, 1)`.
- `_diffusespec` replaces the layer's diffuse, as in OpenMW. Under the `metal roughness` layout it
  is `LAYER_AUTHORED`: A is the roughness, the texture is not delit, and it is a dielectric
  (`DIELECTRIC_F0`). Under the classic layout it is a plain diffuse.
- The lobe of a stack: F0 is `DIELECTRIC_F0` times the share of the weight on authored layers, and
  the roughness is the weighted mean, with a Lambert layer at 1. A stack with no authored layer
  keeps the Lambert numbers exactly, because no division is taken for it.
- Ground reaches `HAS_MAPS` through `Material::mLayersMapped`.
- **Changed from the plan: a flattened distant chunk keeps the geometric normal, but not roughness
  1.** It reads a second baked image, its gloss (R is the share that reflects, G is the roughness).
  The same `groundcomposite.comp` pass bakes it from the same sum (`layerTexel` in `ground.glsl`).
  The reason: some authored ground is smooth (ice at 0.24), and at the grazing angles of distant
  ground the specular albedo is 0.065 to 0.46. With roughness 1 and no reflectance, the edge where
  the ring flattens its chunks would show. Only a chunk with an authored layer gets a gloss, so a
  vanilla chunk pays nothing. Runtime virtual textures do the same: they bake the roughness and the
  specular with the albedo.
- Terrain comes after objects, because each layer adds fetches, and that cost must be measured
  alone.

### 5.11 What is left out, and why

- **AO (B).** Not read at first. The traced bounce and `ambientReaching` already find the
  occlusion of real geometry. The map would count that occlusion twice. An A/B with AO on the
  indirect term only can follow.
- **SSS (A).** Not read at first. The 3204 BC1 maps cannot carry it, so A = 1 there means
  nothing. The existing sheet transmission (`SHEET_TRANSMISSION`) stays the model for leaves.
- **Parallax (`_nh` A).** Built in phase 6, in OpenMW's form: one extra fetch and one offset.
- **Specular aliasing.** First measure whether the mips of the installed normal maps keep the
  averaged length (texconv can renormalize each mip). Toksvig works only on averaged mips.
- **Classic `_spec` maps.** Declined. No physical mapping from a highlight colour to F0 exists.

### 5.12 Cost

- **A vanilla frame pays nothing**, which the first design did not plan: `HAS_MAPS`, a fourth
  question of the kernel tuple, compiles the tangent fetch, the maps' reads and the specular half
  out of a frame whose scene places no mapped material. It was needed for more than cost. With the
  maps' code compiled in, every vanilla view traced a different frame by a rounding — the driver
  fused the Lambert arithmetic around the new code differently — though not one hit ran it.
- **The kernel table doubles on the trace's side**: 16 visibility and 8 froxel launches.
- **Parallax (phase 6)** costs the trace 0.08 to 0.11 ms at Balmora, and nothing measurable at the
  ship or in the guild: one more fetch where a layer or a surface carries a height.
- **Terrain (phase 5)** costs the trace 0.08 to 0.27 ms where ground is in sight with its normal
  maps, and 0.12 to 0.32 with the authored layers as well: a fetch per mapped layer at each hit on
  a stack, and the lobe.
- **`Surface`** grows by 10 floats (`mSmooth`, `mIncident`, `mSpecular`, `mRoughness`) and `Hit` by
  4 (`mTangent`).
- **Memory.** 4 bytes per vertex (phase 2), 8 bytes per material, and the 32 KiB table. Terrain
  (phase 5): 16 bytes per layer row, and 1.4 MB for the gloss of each distant chunk with an
  authored layer (512 × 512 RGBA8 and its chain). The content costs more than the renderer: in the
  23 views, terrain normal maps add 33 to 53 textures and 100 to 300 MiB, and terrain specular maps
  add 70 to 100 glosses and 530 to 1200 MiB. The installed `_diffusespec` land files are 4096²
  DXT5, 22.4 MB each, where the diffuses they replace are 2048² DXT1, 2.8 MB each.

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
5. **Settled in phase 1: the maps load with the models**, on OpenMW's loading threads, as they do
   in GL. There is no pop and no new thread (section 5.4).
6. **Settled in phase 4: the specular bounce goes into the direct channel**, and a separate
   channel waits for the non-RR denoiser.

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

**Phase 1 — maps reach the scene (no picture change).** Done (see section 5.4 for what was built).
- Tests: `ShaderAutoMapsTest` (the `_nh` order, `_n`, no file, the switches, a map the state set
  binds, the bump-map namesake, a drawable's state set, the diffuse's addressing and filtering),
  the format table by encoding, the Vulkan formats, a slot per encoding, the hold and drop of the
  new slots, the data description, the kept roles, the companion maps in the extractor under both
  layouts, and the layout's spellings.
- `rtx debug test`: 535 component, 296 GPU and 20 game tests pass.
- Vanilla against `pbr-baseline/vanilla`: all 23 views, all 184 frame hashes (the scene digest
  included) and all 46 pictures are the same.
- PBR against `pbr-baseline/pbr`: the trace and the pictures are the same on every frame, and all
  46 pictures and map tiles are the same pixel for pixel. The scene differs, because the materials
  name the new slots. From `island-crossing` on the layout also differs between two runs of one
  build, with or without the maps, and `.notes/ISSUES.md` records that.
- Ship view, PBR profile: 1400 textures in 1723 MiB (606 in 860 before), 453 materials wear a
  normal map and 453 a specular map, nothing refused. A debug run under the validation layers
  reports nothing.
- Not built: a `--material-maps` harness switch (a profile with the auto-use switches off is the
  same A/B), and the layout in the launcher and the settings window. The layout is read at load,
  so it needs a restart, and the launcher is where it belongs.

**Phase 2 — tangents (no picture change).** Done (see section 5.5 for what was built).
- Tests: `RtxTangentTest` (the six axes exact with both handedness values and one packed word by
  hand, no length and NaN as none, and every whole direction from −3 to 3 within the bound
  `sqrt(4.5) / 16383` of the steps), `ShaderAutoMapsTest` (the tangents by hand for a square and
  its mirror, a NIF-bound normal map with the rules off read through its own unit, a unit with no
  coordinates, no coordinates at all, a sibling outside the subtree, a rig's source geometry),
  `RtxMeshReaderTest` (unit 7 read as tangents and not as a second set, and the two refusals),
  `RtxSceneDescTest` (the packed word, the zero fill of a reused slot, the byte count),
  `RtxTemplateWalkTest` (a prepared part's run), `RtxSceneDigestTest` (a tangent moves the normals
  column only, zero tangents move nothing), and `RtxSkinPassTest` (the posed word on the device
  for a quarter turn, both handedness values, a direction below the equator against the host's
  packing to the bit, none kept none, and nothing written into a static mesh or by a morph).
- `rtx debug test`: 540 component, 296 GPU and 20 game tests pass.
- Vanilla against `pbr-baseline/vanilla`: every run on a warm pipeline cache has all 184 frame
  hashes and all 46 pictures the same (eleven runs, one of them under 24 busy threads). The first
  run after each change to the ray-tracing shaders primes the cache, starts again, and has a
  different trace at one or two views with the same scene. `.notes/ISSUES.md` records it. The
  kernel with and without the tangent write poses 30 000 random vertices on up to four bones to
  the same position and normal bits.
- PBR against `pbr-baseline/phase1-pbr`: the trace is the same on every frame, and all 46
  pictures and map tiles are the same pixel for pixel. Through `dagoth-ur-caldera` the scene
  differs only in the normals part, which now holds the tangents. From `island-crossing` on, the
  layout nondeterminism of phase 1 also shows.
- Ship view, PBR profile: 2576 meshes carry tangents (none in vanilla). Vertex and index bytes go
  from 40670 to 43275 KiB, and live device memory from 2265.7 to 2284.0 MiB. A vanilla scene
  pays the 4 bytes per vertex too, as zeros, in the host table and in each slot's copy.

**Phase 3 — the model in direct light and in the hit.** Done (sections 5.6, 5.7 and 5.12 say
what was built, and where it left the plan).
- Built as planned: `shaders/brdf.h` (GGX, height-correlated Smith, Schlick with `F90`, the
  compensation), `look.h`'s `DIELECTRIC_F0`, `SPECULAR_EDGE_SCALE` and `ROUGHNESS_FLOOR`, the table
  (`SpecularAlbedo`, a buffer beside the blue noise), `GpuMaterial` at 96 bytes, the tangent fetch,
  the maps' reads, `gather` in two sums, the response, the shared facing function, and
  `rtxtool --show=albedo|normal|roughness|specular` (it replaces `--albedo`).
- Left out of the plan: the visible-normal sampler in `brdf.h` (the table has its host copy; the
  shader's belongs to phase 4, which draws with it); the material flags (a specular slot is what
  says a material is not delit, and a blue of nought says a map has two channels); a `metal` view
  (`specular` shows F0, where a metal shows its base colour).
- Tests: `RtxBrdfTest` (a reflectance of nought reflects exactly nothing, reciprocity, the
  distribution's normalisation), `RtxSpecularAlbedoTest` (the table against a 500-step quadrature
  that shares nothing with it, within 3e-4; the mirror limit at the roughness floor; Ray
  Reconstruction's `EnvBRDFApprox2` within 0.06 for cosines from a half, the fit being to another
  lobe's table), the mesh flag through a reused slot, the mapped count through reclasses, the digest
  of the flag, and the device test of section 5.7 with the three views.
- `rtx debug test`: 546 component, 297 GPU and 20 game tests pass.
- Vanilla: with `--upscale=off`, 17 of 23 views have every frame hash the same as the phase 2 logic
  over the same host, and the saved pictures of all 23 are the same byte for byte. The six others
  are the run-to-run set of `.notes/ISSUES.md`; one frame hash of `balmora`'s picture moved on two
  of eight frames. Against the DLSS baseline, the same set moves by the network's noise.
- PBR: 21 of 23 views change (the other two place no mapped material in view). The census office
  guard's steel is dark metal with lamp highlights — the room's reflection arrives with phase 4 — and
  the stone and the tapestries show their relief. Direct light only: a metal in shade is dark.
- `bench`, the 2026-09-25 entry of `.notes/bench.txt`: on the PBR profile the trace costs 0.7 ms
  more at the ship (5.27–5.34 to 5.98–6.05), 0.35 in the guild and 0.5 at Balmora, and the frame
  follows it. Vanilla is inside the spread at every place. The p99 and the worst frame are the
  desktop's in both arms.

**Phase 4 — the specular bounce.** Done (section 5.8 says what was built, and where it left the
plan).
- Built: `visibleNormal`, `smithShadowingGivenMasking` and `ggxConeWidth` in `brdf.h`, with
  `normalize`, `atan` and `min` on the host's side of the shared headers; `SEED_BOUNCE_LOBE`;
  `lobeSample` and `fresnelAt` in `gloss.glsl`; `bounceDraw`, `bounceArriving` and the two halves
  of `Bounce` in `shading.glsl`, and the lobe's half into the direct light in `shadeSolid`.
- Tests: `theVisibleNormalsAreDrawnByTheirDensity` (2^18 Hammersley draws a case against the
  density integrated over each of 256 bins, within 2e-4 over nine eyes and roughnesses, where a
  draw that ignores the eye is 5e-3 to 5e-2 off), `theSmithTermsAreReciprocalAndAgree` (`G2 / G1`
  against Smith's `Λ`), `theConeWidthIsWhereTheDistributionFallsToHalf`, and the device's white
  furnace, `aGlossyFloorUnderAnEvenSkyGivesBackWhatItReflects`: a white metal gives an even sky
  back within 3e-5, and half a metal both of its halves within 2.6e-4. The table test's quadrature
  moved to `lobeIntegrals` for it. **The weight at F0 = 0 is no weight at all**: a Lambert surface
  draws nothing new, which the bounce tests and the vanilla shots hold exactly.
- Vanilla: 9 of the 23 views, every interior among them, are the same as the phase 3 baseline
  frame for frame, with and without DLSS. **The other 14 move in the direct light alone**, where one
  sky source holds all the weight: phase 3 divided that source's light by its weight over itself,
  which this device does not always round to one, and `gather` now takes it whole. At most four
  pixels in two million move by one byte, in 8 of the 23 pictures without DLSS. The indirect light,
  which the bounce's far hit shades through the same code, is the same frame for frame. **Decided:
  the one path stays and the rounding goes**, which is the one place this phase moves a vanilla
  picture; the two calls kept the rounding and ran the far hit twice in a mixed warp.
- PBR: 21 of 23 views change. The census office guard's steel reflects the room, where phase 3
  left a metal in shade dark. The mean moves by -1.2 to +3.8 per cent.
- `bench`, the phase 4 entry of `.notes/bench.txt`: on the PBR profile the trace moves by under a
  tenth of a millisecond at the ship, the guild and Balmora, and vanilla is inside the spread. The
  far hit with a call per path cost 0.85 to 1.2 ms of trace, which is what the one path saves. The
  tail does not tell the arms apart.
- `rtx debug gate`: 550 component, 299 GPU and 22 game tests pass, 47 checks, and
  `repeat --pairs=10` is identical.

**Phase 5 — terrain.** Done (section 5.10 says what was built, and where it left the plan).
- Built: `GpuLayer::mNormal`, `mFlags` and `LAYER_AUTHORED`; `PreparedLayer`'s normal map and
  `mDiffuseSpec` from `GroundReader`; the normal slot and the flag in `CellPlacer`, which takes the
  layout from the ring; `Material::mLayersMapped`; `layerTexel` in `ground.glsl`; the stack's maps
  in `resolveFor`; the gloss in `CompositeQueue` (`TextureSource::GroundGloss`, `Baked`) and in
  `groundcomposite.comp` (`GROUND_COMPOSITE_GLOSS`); and the fields in the scene digest, hashed only
  where they are set.
- Fixed on the way: `MaterialTable::forEachTexture` held a layer's diffuse and not its normal map,
  so the slot was never freed. The ring's teardown test found it.
- Tests: `groundSumsItsLayersMapsByTheWeightsItsAlbedoIsSummedBy` (the device's normal, roughness
  and reflectance at three columns of a two-layer card against the host's frame, and a flattened
  chunk's gloss), `theCompositeAndItsGlossAreTheStackSummedAtTheLevelTheFootprintCallsFor` (the gloss
  texel for texel at two mip levels), the gloss slot in `RtxCompositeQueueTest`, the maps in
  `RtxGroundReaderTest`, the slot, the flag under both layouts and the unflattened chunk in
  `RtxCellRingTest`, and the layer normal's hold in `aTextureGoesWithTheLastMaterialThatNamesIt`.
- `rtx debug test`: 556 component, 300 GPU and 22 game tests pass.
- Vanilla against the phase 4 state (`--upscale=off`): all 23 views and all 184 frame hashes are
  the same. The kernels verb moves every `visibilityhit` tuple and `groundcomposite`: the layer
  row's type changed, and the vanilla tuples differ only by that, one dead compare and one vector
  fold that the optimizer missed.
- PBR: the 15 views with ground in sight change, and the 8 others keep their trace. With `auto use
  terrain specular maps` on (not in the profile), the same 15 change again: the ground shows a sheen
  at grazing angles.
- `rtx debug repeat --pairs=10`: identical on vanilla, and on the PBR profile with both terrain
  switches on.
- `bench`, the phase 5 entry of `.notes/bench.txt`, against the phase 4 tree built aside: terrain
  normal maps cost the trace 0.08 to 0.27 ms where ground is in sight, and the authored layers on
  top of them 0.12 to 0.32. Vanilla is inside the spread. The 4096² `_diffusespec` files cost the
  trace nothing measurable. The tail does not tell the arms apart.
- `rtx debug gate`: 556 component, 300 GPU and 22 game tests pass, 47 checks, and the repeat pair
  is identical.

**Phase 6 — parallax on `_nh`.** Done.
- Built: OpenMW's offset, `eye.xy * (height * 0.04 - 0.02)` with the eye in the tangent frame
  (`PARALLAX_SCALE`, `PARALLAX_BIAS`, `parallaxShift`). A surface takes it where
  `Material::mParallax` (`MATERIAL_PARALLAX`) is set: its normal map was bound as
  `normalHeightMap` (`SurfaceDescription::mNormalHeight`) and `carriesHeight`, and the material is
  no cutout. A ground layer takes it under `LAYER_PARALLAX`, set where the storage found an `_nh`
  that `carriesHeight`. `carriesHeight` is false for BC5, as OpenMW's visitor and terrain turn
  parallax off for two-channel maps. The height is read at the point before the shift, and a
  normal map that stands in shifts nothing.
- **Every read on the shifted point's coordinate set takes the shift**, the specular, emissive and
  dark maps included. OpenMW shifts its diffuse and its normal map alone, because its other maps
  may read another coordinate set; here each read knows its set, and a specular map left behind is
  roughness painted for another texel of the colour.
- **No cutout takes it.** The traversal's any-hit test finds a hole with no eye to shift toward,
  so a shifted cutout would shade the leaf at one place and cut the hole at another. OpenMW shifts
  its alpha test too, so a cutout with an `_nh` is the one surface that draws flatter here.
- **One path.** `resolveFor` serves the primary hit and the bounce's far hit alike, so the far
  hit shifts too. The plan's "primary rays first" was an order of work, and the bench measures the
  whole cost.
- Tests: `parallaxShiftsTheSheetTowardTheEyeByTheNormalMapsHeight` (a surface and a ground layer
  shifted by the hand-computed `±0.014142` at the full and at no height, within 5e-5, and nothing
  where the normal map stands in), the height bit and BC5 in `RtxSurfaceTest`, the material's
  parallax and the cutout in `RtxSceneExtractorTest`, the layer's in `RtxGroundReaderTest` and
  `RtxCellRingTest`, and the field in `everyMaterialFieldReachesBothDigests`.
- `rtx debug test`: 556 component, 303 GPU and 22 game tests pass.
- Vanilla against HEAD: all 23 views and all 184 frame hashes are the same. The kernels verb
  moves every `visibilityhit` tuple, the vanilla ones by instruction order alone: the layer row's
  flags word is loaded earlier, where the parallax test reads it, and one undefined value. The
  layer frame is filled behind `HAS_MAPS` and not beside it, because the verb's optimizer keeps
  arithmetic nothing reads.
- `rtx debug gate`: 556 component, 303 GPU and 22 game tests pass, 47 checks, and the repeat pair
  is identical.
- `bench`, the phase 6 entry of `.notes/bench.txt`, against HEAD built aside: 0.08 to 0.11 ms of
  trace at Balmora on the PBR profile, where the ground's layers read their heights, and the ship
  and the guild inside the spread. Vanilla is inside the spread. The tail does not tell the arms
  apart.
- PBR: 21 of 23 views change. The mods ship 1189 `_nh` files, all DXT5. The shift is at most a
  fiftieth of a repeat, as in OpenMW, so the pictures differ mostly by the sampling noise.

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
