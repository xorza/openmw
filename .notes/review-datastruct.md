# Data-structure review: the fork against upstream master

Scope: every production file the fork adds or changes against `upstream/master`. Tests and the
APIs only tests use are left out.

**Delete an item when you have addressed it.** Delete a heading when its last item goes. This
file lists open findings only.

Groups are named after the root cause and sorted by severity and benefit.

## Parallel arrays where a row belongs

- [ ] `BottomLevelStore::mBuildSizes`, `mBuildScratchOffsets`, `mArrivedAt`
      (`components/rtxvulkan/bottomlevelstore.cpp`) are three per-build arrays sized separately to
      `meshes.size()` and read together.

## One census struct carrying three things

- [ ] `ExtractionStats` (`components/rtx/extractionstats.hpp`) is a walk census (eighteen
      counters), a timing (`mFoldMs`) and a texture-format histogram (`mTextureFormats`,
      `mUnnamedFormat`). `operator+=` (`extractionstats.cpp`) reaches the counters through a
      twenty-one-name structured binding that has to be edited in step with the member order.
- [ ] `RtxRenderer::mFound` / `mFoundAgain` and `FrameReport::mWalked` / `mWalkedAgain` hold the
      first and second walk's stats as two named members in two places. A second walk is a
      property of one walk report, not a second report.

## A material in two shapes across the surface seam

`Surface::Material` and `Rtx::Material` describe one surface, and the two use different types for
the same fields, so every crossing converts.

- [ ] `Surface::Colour` (`components/surface/colour.hpp`) is an RGB triple while the rest of the
      tree, `Rtx::Material` included, uses `osg::Vec3f`. One type for one colour.
- [ ] `Surface::Material::mTextureScale` + `mTextureOffset` (two `Vec2f`) and
      `Rtx::Material::mTextureTransform` (`Vec4f`) are one texture transform in two shapes.
- [ ] `Surface::sTextureRoleCount = 11` and `Sky::sDayPhaseCount = 5` are counts stated beside the
      enums they count, and nothing ties them.
- [ ] `MeshRange::mMaterial` (the material a mesh arrived wearing) and `MeshInstance::mMaterial`
      (the material it wears now) are two answers to "what material is on this surface", and the
      extractor compares them per placement per frame (`sceneextractor.cpp`). What the mesh row
      needs is the mask it was baked against, not a material index that can drift.

## The sun stated four times, and conditions stored beside the struct that holds them

- [ ] The sun's position and irradiance are carried by `Sky::SunPlacement`, `Rtx::SkyReading`,
      `Rtx::Sun` (twice in `Skylight`, `mSun` and `mSunAloft`) and again as three loose members
      `OffscreenTrace::mSunPosition`, `mSunIrradiance`, `mAmbient` (`offscreentrace.hpp`). The
      loose three are a `Skylight` or a `SceneUtil::FlatLight`, which `setLight` already takes.
- [ ] `Weather::Precipitation` (`components/weather/precipitation.hpp`) stores `mEye`,
      `mStormDirection`, `mUnderwater` as three members and takes the same three as a
      `Conditions`. One `Conditions mWhere` member is the same state with one shape.
