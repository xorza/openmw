#Open issues

- `SkinPass::recordArrived` writes an arrived
    mesh's bone and weight rows with `Buffer::appendAt`, on the promise that a run only just handed out is one no
        placement in flight reads.Whether the deformer table can hand a pose run freed this frame to an arrival in the
            same frame,
    while the placement that posed the freed mesh is still on the queue, is not checked anywhere.If it can,
    the arrival rewrites rows that placement's dispatch reads.
