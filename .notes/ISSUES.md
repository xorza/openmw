# Open issues
- `RtxSceneDescTest.aMeshRemembersWhereItsVerticesWent` reads `scene.getMeshes()[scene.addMesh(...)]`: the span is taken before `addMesh` grows the table, so it indexes freed memory and the `mSheet` assertion passes or fails with the heap's mood.
