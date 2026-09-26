# Glossary

The words this tree uses, beside the words the field uses for the same thing, and the class
that owns each. Read this once and the headers read as the field's.

| here | elsewhere | owner |
|---|---|---|
| walk, mirror | traversal, extraction | `SceneExtractor`, `MirrorTraversal` |
| stand, stood, standing | an instance is live, is placed | `PlacementTable`, `CellPlacer` |
| re-stood | re-instanced under a new slot | `ExtractionStats::mRestood` |
| place, placement | instance, per-frame instance update | `SceneDesc::addInstance`, `Renderer::placeScene` |
| hand over, hand, handing | upload, submit | `SceneUploader::hand`, `WorldMirror::hand` |
| hold, holds, drop | reference count, pin, unpin | `SlotRows::hold`, `Kept::hold` |
| lend, lent, give back | borrow from a pool, return to it | `Spares`, `CellSupply` |
| spare, recycled | free-list entry | `Spares`, `Recycled` |
| epoch | generation, mark of one traversal | `MirrorPass::mEpoch` |
| stamp, reached | marked live this generation | `Kept::stamp` |
| whole | no stale entry, sweep skippable | `Kept::whole` |
| retire, sweep | mark-and-sweep collection | `SceneExtractor::retire`, `SlotRows::sweep` |
| turn, phase, step | state machine, lifecycle assert | `Stepped`, `RtxRenderer::Phase` |
| the ring | residency set, streaming window | `CellRing` |
| supply, reader | streaming thread and its queue | `CellSupply`, `CellReader` |
| prepared | decoded off the content files, not yet uploaded | `PreparedCell`, `PreparedModel` |
| slot | index into a fixed-row table | `SlotRows`, `SlotPool`, `SceneSlot` |
| run | contiguous range in one buffer | `Run`, `RunAllocator` |
| bury, graveyard | deferred destruction, in-flight garbage | `Graveyard`, `Retiring` |
| timeline | timeline semaphore and the value it reached | `Timeline` |
| frame behind, in flight | the previous frame on the device | `FrameRing` |
| collect, finish | read back a frame's report, wait for it | `Renderer::collectFrame`, `finishFrame` |
| spend | per-frame timing breakdown | `FrameSpend` |
| stop | checkpoint of a measured run | `RtxTool::Stop`, `RtxTool::StopWriter` |
| a picture | render-to-texture view for the interface | `OffscreenTrace`, `TracedView` |
| a subject | the doll or the race preview in a scene of its own | `SubjectView` |
| the puffs, sprites | particle billboards | `SpriteBin`, `spriteshade.comp` |
| sheet | a texture read as one shading map | `ShadingMap` |
| fold, folded shape | a mesh's shape reduced to a hash | `ShapeFold` |
| knobs | the mirror's settings | `MirrorKnobs` |
| profile | the run's rendering settings | `RenderProfile` |
| pin, pinned | float arithmetic every compile computes alike, invariance | `Rtx::pinFloatArithmetic` |
