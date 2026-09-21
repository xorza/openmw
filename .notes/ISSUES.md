# Open issues
- `components/sceneutil/paintedtexture.cpp:95` compiles with `-Wmissing-field-initializers` (`SceneUtil::Painted::mRegion`) in the `Release` build of `components`, which `rtx debug gate` never compiles: its release leg builds the two RTX libraries alone.
