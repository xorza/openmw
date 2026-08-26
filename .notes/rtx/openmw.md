# OpenMW as a host engine

Structure notes for the experimental ray-traced renderer. This describes the tree **as it is**, and
marks the seams the new renderer attaches to. It is not a plan — that is `.notes/rtx/plan.md` — and not a
statement of intent — that is `CLAUDE.md`.

OpenMW 0.52.0, C++20, ~24 k lines of world simulation and ~23 k lines of rendering. The renderer is
OpenSceneGraph 3.6 on OpenGL, driven by an `osgViewer::Viewer`. Nothing in the tree knows about
Vulkan.

---

## 1. The shape of the process

`apps/openmw/main.cpp` → `OMW::Engine::go()` → a plain loop, `apps/openmw/engine.cpp:1041`:

```
while (!mViewer->done() && !mStateManager->hasQuitRequest())
    frame(frameNumber, dt);
```

`Engine::frame` (`engine.cpp:191`) is the whole tick, in order: input, sound, Lua sync, state,
scripts, mechanics, physics, world, GUI — then

```
mViewer->eventTraversal();
mViewer->updateTraversal();     // animation, controllers, RigGeometry bounds
mViewer->renderingTraversals(); // cull + draw, engine.cpp:360
```

Everything before `updateTraversal` is simulation and produces no pixels. **`renderingTraversals()`
is the single call the new renderer displaces.** Cull is not optional even so — see §3.4.

Startup order that matters (`Engine::prepareEngine`, `engine.cpp:729`):

| step | line | note |
|---|---|---|
| `Stereo::Manager` | 739 | |
| root `osg::Group` set as scene data | 743 | |
| `createWindow()` | 745 | SDL window + OSG graphics context + `mViewer->realize()` |
| `VFS::Manager` + archives | 747 | BSA + loose files |
| `Resource::ResourceSystem` | 751 | scene, image, NIF, keyframe, bulletshape managers |
| `setUnRefImageDataAfterApply(false)` | 754 | **CPU image data stays resident** — a gift, see §4 |
| work queue, screenshot handler, L10n, Lua | 757+ | |
| `MWWorld::World` | 847 | constructs `RenderingManager` at `mwworld/worldimp.cpp:248` |

`createWindow()` (`engine.cpp:500`) asks SDL for `SDL_WINDOW_OPENGL`, sets GL attributes, wraps the
window in an `osgViewer::GraphicsWindow`, and runs a sequence of *realize operations* — GL
identification, extension query, optional `GL_KHR_debug`, depth/colour format selection, stereo.
A Vulkan window would be a fork at this point; a Vulkan **device with no surface** needs nothing
here at all.

---

## 2. The rendering god-object

`MWRender::RenderingManager` (`apps/openmw/mwrender/renderingmanager.hpp`, 373 lines of interface)
is owned by `MWWorld::World` and reached as `mRendering->` from about 74 call sites, almost all in
`mwworld/worldimp.cpp`, `mwworld/scene.cpp`, `mwworld/weather.cpp` and `mwworld/projectilemanager.cpp`.

There is exactly one abstract interface in the rendering layer, and it has one method
(`mwrender/renderinginterface.hpp`):

```cpp
class RenderingInterface { virtual MWRender::Objects& getObjects() = 0; };
```

So there is **no renderer abstraction to implement**. The seam has to be cut, not found.

What `RenderingManager` actually owns, and what each part means for us:

| member | file | lines | fate |
|---|---|---|---|
| `mSceneRoot` (`SceneUtil::LightManager`) | `sceneutil/lightmanager.cpp` | 755 | **source of truth** — the scene graph we mirror |
| `mObjects` | `mwrender/objects.cpp` | | inserts per-reference nodes; keep |
| `mTerrain` (`Terrain::World`) | `components/terrain/` | 3.9 k | keep; read its chunks |
| `mObjectPaging` | `mwrender/objectpaging.cpp` | 1.1 k | keep; merged statics are ideal BLAS input |
| `mGroundcover` | `mwrender/groundcover.cpp` | | keep |
| `mSky` (`SkyManager`) | `mwrender/sky.cpp` + `skyutil.cpp` | 2.0 k | **replace** — sky becomes an analytic/traced dome |
| `mWater` | `mwrender/water.cpp` | 873 | **replace** — see `.notes/rtx/plan.md` M6 |
| `mFog` (`FogManager`) | `mwrender/fogmanager.cpp` | | replace; keep it as the *data* source |
| `mPostProcessor` | `mwrender/postprocessor.cpp` | 880 | **bypass** — the RT path owns tonemapping |
| `mShadowManager` | `sceneutil/mwshadowtechnique.cpp` | 3.4 k | **delete from the RT path** — rays cast shadows |
| `mCamera` | `mwrender/camera.cpp` | | keep; read the view matrix from it |
| `mPlayerAnimation`, `mEffectManager` | | | keep |
| `mScreenshotManager` | `mwrender/screenshotmanager.cpp` | | GL readback; RT path needs its own |
| `castRay` / `IntersectionVisitor` | `renderingmanager.cpp` | | **keep as-is** — gameplay picking is CPU, not GPU |

`configureFog`, `setSunDirection`, `setSunColour`, `setAmbientColour`, `configureAmbient`,
`setWaterHeight`, `setWaterEnabled`, `setNight`, `skyGetMasserPhase` are all **inputs the game hands
the renderer every frame**. They are the environment description, already computed, already correct,
and they arrive at a single class. That set is the API the RT renderer should consume too.

---

## 3. What the scene graph already carries

This is the reason to fork OpenMW rather than start from the ESM files: by the time the frame runs,
the world is already a flat, traversable, CPU-resident description of every visible triangle.

### 3.1 Geometry

`osg::Geometry` with `osg::Vec3Array` positions and normals, `osg::Vec2Array` UVs, `osg::Vec4Array`
tangents where a normal map required them, and `osg::DrawElements*` indices. All in system memory,
all reachable by a `NodeVisitor`. Buffer objects are for GL's benefit; the arrays outlive them.

### 3.2 Transforms

`SceneUtil::PositionAttitudeTransform` and `NifOsg::MatrixTransform` down to the drawable. World
transforms fall out of `osg::computeLocalToWorld(nodePath)` during any traversal.

### 3.3 Skinning is done on the CPU, and the result is readable

`SceneUtil::RigGeometry` (`components/sceneutil/riggeometry.hpp:28`) holds
`osg::ref_ptr<osg::Geometry> mGeometry[2]` and `getGeometry(frame)` returns the deformed geometry for
that frame. Skinning runs in `RigGeometry::cull()`. **Deformed vertex positions for every animated
actor are available in system memory each frame** — exactly the input a BLAS refit wants, with no
GPU skinning pass to write.

Cost note: it is also a per-frame memcpy per actor. Refit reads it; do not copy it twice.

### 3.4 Cull cannot be skipped

`RigGeometry::accept` (`riggeometry.cpp:369`) does `static_cast<osgUtil::CullVisitor*>(&nv)` and only
skins under `CULL_VISITOR`. Terrain LOD selection (`Terrain::QuadTreeWorld`) and object-paging chunk
selection are likewise cull-time decisions. So the RT path must still run a real `osgUtil::CullVisitor`
over the scene — it just must not run a GL draw afterwards. Culling is cheap and does not touch the
GL context.

### 3.5 Materials

`Shader::ShaderVisitor` (`components/shader/shadervisitor.cpp:255`) has already resolved every NIF
texture slot into a named role on the `osg::StateSet`:

```
diffuseMap normalMap emissiveMap darkMap detailMap envMap specularMap decalMap bumpMap glossMap
```

plus `alphaRef`, `colorMode`, `normalHeightMap`, and the `osg::Material` for ambient/diffuse/emissive
colour. Alpha test/blend live in the stateset. That is the whole vanilla material model, pre-parsed.

Where the stateset is not enough (parallax flags, `NiStencilProperty` two-sidedness, the raw
`NiTexturingProperty` apply mode), the original record is one lookup away: `components/nif/` is a
**pure parser** — its only dependency outside `components/` is osg's header-only math (`osg::Vec3f`,
`osg::Matrixf`, `osg::Quat`). No GL, no scene graph.

### 3.6 Lights

`SceneUtil::LightManager` is the scene root and holds every light source with its
`SceneUtil::LightSource` / `LightController` (flicker, pulse). `mSunLight` is separate. Light lists
are computed per-drawable during cull for the clustered-lighting path
(`sceneutil/clusteredlighting.hpp`) — an RT light grid can consume the same source data without the
clustering.

### 3.7 Environment inputs

| quantity | source |
|---|---|
| sun direction, colour, visibility | `MWWorld::Weather` → `RenderingManager::setSunDirection/setSunColour` |
| ambient | `configureAmbient(cell)` from the cell's `AMBI` record |
| fog colour/depth, distant-land factor/offset | `MWRender::FogManager`, `mwworld/weather.cpp` |
| water level | `MWWorld::Cell` water height → `setWaterHeight` |
| moon phases, cloud texture, weather blend | `MWRender::SkyManager` |
| time of day, game hour | `MWWorld::DateTimeManager` |

`mwworld/weather.cpp` is the one place the ini fallbacks (`Weather_*`, `Moons_*`, `Fog_*`) have
already been read and interpolated. rtxmw had to re-derive all of this from `Morrowind.ini`
(`docs/design.md` §8.59); here it is a getter.

---

## 4. Below the scene graph: the data path

```
BSA / loose files            components/bsa/, components/vfs/
      ↓
VFS::Manager                 registerArchives(), case-folded normalized paths
      ↓
Resource::NifFileManager  →  Nif::NIFFile           components/nif/          (pure parser)
Resource::ImageManager    →  osg::Image             DDS/TGA/BMP, mips intact
      ↓
NifOsg::Loader            →  osg::Node              components/nifosg/       (5.6 k lines)
      ↓
Resource::SceneManager    →  cached template + per-instance clone, shader visitor applied
```

Two properties of this pipeline matter:

1. **`setUnRefImageDataAfterApply(false)`** is already set at `engine.cpp:754`, with the comment
   "keep to Off for now to allow better state sharing". Consequence: every `osg::Image` keeps its
   CPU pixels for the lifetime of the texture. Texture upload to Vulkan is a memcpy from a pointer we
   already hold, not a re-decode.
2. **Nothing in the chain needs a GL context.** Contexts are needed to *draw*, not to load.

`Terrain::TerrainDrawable` (`components/terrain/terraindrawable.hpp:51`) does not put its material on
the graph. It carries a `PassVector` — one `osg::StateSet` per ground layer, each holding the layer's
tiling texture at unit 0 and its blend map at unit 1, drawn over the same triangles with additive
blending and equal-depth ordering (`components/terrain/material.cpp:200`). A node-state walk finds
nothing on a chunk, so anything reading terrain shading has to ask the drawable. The two texture
matrices are `texMat0` and `texMat1` uniforms, transposed on the way to GLSL.

`components/esmloader/` (`EsmLoader::loadEsmData`) reads content files into flat vectors with no
world simulation attached — this is how the non-graphical tools work. Which record types it reads is
`EsmLoader::ModelRecords`, one list that the reader, the store, the query mask and `getModel` are all
generated from; a caller names the subset it wants with `modelRecords<...>()`, because what gets read
decides what its world contains. The navmesh tools ask for the four types that carry collision and
the RT harness asks for everything, which is a third of an interior's references either way.

---

## 5. Headless precedent

Two shipped tools already load Morrowind's world with no window, no GL, and no `MWWorld::World`:

- `apps/navmeshtool/` — `main.cpp:201` builds a `VFS::Manager`, `main.cpp:227` calls
  `EsmLoader::loadEsmData`, `worldspacedata.cpp:135` `forEachObject` walks a cell's refs resolving
  each model through the VFS, and hands them to `Resource::BulletShapeManager`.
- `apps/bulletobjecttool/` — the same shape, smaller.

Swap `BulletShapeManager` for `SceneManager` and this is a static-geometry loader for the RT harness.
It gives no animation, no cell streaming and no weather, which is the trade: fast, deterministic,
and unable to test anything the simulation drives.

---

## 6. Settings

A setting exists in **four** places and the build enforces it:

1. `components/settings/categories/<category>.hpp` — a `SettingValue<T>` with its category string,
   key string and optional sanitizer. `values.hpp` aggregates the categories into `Settings::Values`.
2. `files/settings-default.cfg` — the default, under a `[Category]` header, with a comment.
3. `docs/source/reference/modding/settings/<category>.rst` — user documentation.
4. `apps/components_tests/settings/testvalues.cpp` — the test loads `settings-default.cfg` and
   checks the declarations against it. A setting declared without a default fails the suite.

Reading is `Settings::video().mResolutionX` or `Settings::get<bool>("Category", "key")`; writing is
`.set(v)`. Changed settings are broadcast as `Settings::CategorySettingVector` to
`RenderingManager::processChangedSettings`.

Two front-ends:

- **Launcher** (Qt): `apps/launcher/graphicspage.cpp` + `ui/graphicspage.ui`, hand-written
  load/save against `Settings::video()`. Everything here is applied on next start.
- **In-game** (MyGUI): `files/data/mygui/openmw_settings_window.layout` binds widgets to settings
  declaratively —

  ```xml
  <UserString key="SettingCategory" value="Video"/>
  <UserString key="SettingName"     value="framerate limit"/>
  <UserString key="SettingType"     value="Slider"/>   <!-- or CheckButton -->
  <UserString key="SettingValueType" value="Float"/>
  ```

  read back by `MWGui::SettingsWindow` (`settingswindow.cpp:75-90`, applied at `:672` and `:742`).
  A new checkbox costs four XML lines and no C++.

---

## 7. GUI, and what depends on the GL renderer

MyGUI is bound to OSG by `components/myguiplatform/` (1.5 k lines): `myguirendermanager.cpp` is the
whole GL-side contract — create texture, create vertex buffer, `doRender(buffer, texture, count)`.
A Vulkan MyGUI backend would be a sibling file of the same size. **Not needed** if the RT image is
composited under the existing GL GUI (`.notes/rtx/plan.md` §3).

Other consumers of the GL renderer, all rendering into textures rather than to the screen:

| feature | file | approach |
|---|---|---|
| character preview / inventory doll | `mwrender/characterpreview.cpp` | RTT camera |
| local map | `mwrender/localmap.cpp` | RTT camera, top-down |
| global map | `mwrender/globalmap.cpp` | RTT + CPU compositing |
| video playback | `extern/osg-ffmpeg-videoplayer` | decodes into `osg::Texture` |
| loading screen | `mwgui/loadingscreen.cpp` | drives the viewer directly |

Each is small, none is on the main view path, and all keep working unchanged as long as a GL context
still exists. That is the single strongest argument for the interop route in `.notes/rtx/plan.md` §3.

---

## 8. Build system

- CMake ≥ 3.16, C++20, one big `components` static library (`components/CMakeLists.txt:587`) built
  from `add_component_dir(<dir> <file> …)` entries.
- `apps/openmw` builds **`openmw-lib`** (static, all of `mw*/`) plus a thin `openmw` executable
  (`apps/openmw/CMakeLists.txt:119`). Tests link `openmw-lib`. Any new app can too.
- Executables use the `openmw_add_executable` macro (`cmake/OpenMWMacros.cmake:114`) and are named
  `openmw-<tool>` — `openmw-navmeshtool`, `openmw-bsatool`.
- Feature flags are plain `option()` in the root `CMakeLists.txt:31-48`; `BUILD_*` for targets,
  `OPENMW_USE_SYSTEM_*` for dependencies.
- Tests are two gtest binaries, off by default: `components-tests` (`BUILD_COMPONENTS_TESTS`) and
  `openmw-tests` (`BUILD_OPENMW_TESTS`). **No `add_test`, no ctest** — the binaries are run directly
  and filtered with `--gtest_filter`.
- Benchmarks: `apps/benchmarks/`, Google Benchmark, `BUILD_BENCHMARKS`.
- Formatting: `.clang-format` (LLVM base, 120 columns, C++20); CI pins **clang-format 14**
  (`.gitlab-ci.yml:201`) while this machine has 22 — they disagree, so format with 14 or accept
  churn. `.clang-tidy` exists and CI can run it as `CMAKE_CXX_CLANG_TIDY`.

---

## 9. The seams, in one table

| we take | we replace | we must not touch |
|---|---|---|
| cell streaming, refs, physics, scripts, Lua | primary visibility | gameplay ray casts (CPU intersection) |
| NIF → `osg::Geometry`, texture decode, VFS | direct + indirect lighting | save games, ESM writing |
| CPU skinning, animation controllers | shadows (`mwshadowtechnique`) | the launcher's other pages |
| terrain quadtree, object paging, groundcover | sky, water, fog | MyGUI layouts other than the settings additions |
| weather, sun, ambient, water level, time | tonemap / post-process chain | |
| the settings/launcher/GUI plumbing | the swap chain and the draw | |

## 10. Numbers worth knowing

| | |
|---|---|
| `apps/openmw/mwworld` | 24,872 lines |
| `apps/openmw/mwrender` | 22,959 |
| `components/esm3` | 18,251 |
| `components/sceneutil` | 16,342 |
| `components/nif` | 11,890 |
| `components/resource` | 6,012 |
| `components/nifosg` | 5,640 |
| `components/terrain` | 3,855 |
| `components/settings` | 3,577 |
| `components/myguiplatform` | 1,529 |
