# Open issues

- `VulkanRenderer::renderFrame` names `mUpscaled` outside `#ifdef OPENMW_RTX_DLSS` when it picks
  the image the puffs are composited over, while the member is declared inside it, so a build
  configured with `-DOPENMW_RTX_DLSS=OFF` does not compile.

- The picture depends on where the device allocations sit: with `CompositePass`, `SpriteBinPass`
  and `SpriteShadePass` constructed before the two `TraceChain`s instead of after them — the same
  code, the same inputs to every shader — `shot --views=all --upscale=off --filter=false
  --exposure=1` moves one pixel of `dagon-fel` and two of `island-crossing-end` by 1 of 255.
  With only `VisibilityPass` moved ahead nothing moves, so what moves the chains' `SpriteBin`
  buffers by one small allocation (the composite's 1×1 stand-in image) is what changes the
  picture. `repeat` cannot see it, because two runs of one binary allocate alike.

- The release build (`build-release`, `-O3 -DNDEBUG`) does not compile at HEAD:
  `components/rtx/texturetable.cpp:96` fails `-Werror=null-dereference` in `TextureTable::drop`,
  where `known->second` follows an `assert` that `-DNDEBUG` compiles out.
