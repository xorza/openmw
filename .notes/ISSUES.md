# Open issues

`MWRender::RtxRenderer::renderFrame` builds a `Rtx::FrameOptions` that leaves `mSinceLast`
uninitialised, and the openmw target warns on it (`-Wmissing-field-initializers`,
`rtxrenderer.cpp:593`).
