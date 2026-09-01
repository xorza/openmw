# Open issues

A low-poly mesh whose vertex normals lean far off their own facets loses whole triangles of direct
light in a hard-edged patch. `terrain_rock_bc_18.nif` at -2,-9 (-8536, -9810, 418) does it under noon
sun: its worst vertex normal is 46° off its face, and two triangles whose planes face away from the
sun go to zero while their neighbours take the full cosine. `litCosine` reads the plane for the side,
and the sun's shadow ray leaves a point whose facet faces away and hits the rock. Removing either
alone leaves the other; removing both clears the patch.

Neither is removable as the code stands. The plane gate is what stops a single unbacked quad — a
leaning floor, a wall panel — lighting itself from behind, and `GpuMesh::mSheet` does not name one:
`SheetFold` only marks geometry the content doubled, so a one-sided quad reads as a solid.
`RtxVisibilityTest.aLightBehindASurfacesOwnTriangleDoesNotReachIt` is that case. Hanika's shadow
terminator lift answers the shadow ray, but Morrowind's triangles are wide and their corner normals
splay, so the smooth surface they imply sits far from the geometry: an unbounded lift moved 94% of
Balmora's pixels and banded every flat parapet in Vivec, and taking the corners' mean tilt out first
still moved 90%.
