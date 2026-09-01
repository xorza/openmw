# Open issues

A low-poly mesh whose vertex normals lean far off their own facets loses whole triangles of direct
light in a hard-edged patch. `terrain_rock_bc_18.nif` at -2,-9 (-8536, -9810, 418) does it under noon
sun: its worst vertex normal is 46° off its face, and two triangles whose planes face away from the
sun go to zero while their neighbours take the full cosine. `litCosine` reads the plane for the side,
and the sun's shadow ray leaves a point whose facet faces away and hits the rock. Removing either
alone leaves the other; removing both clears the patch.
