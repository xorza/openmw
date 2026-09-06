# Open issues

- Two `bench --views=island-crossing --hashes` runs of one build differ on 355
  of 360 frames. `Rtx::CompositeQueue::setSettled` accounts for 4 of them, so
  the terrain bake is not what makes a moving run unrepeatable.
  `Rtx::FrameHashes` and `Rtx::Renderer::mTextureCount` both claim it is.
