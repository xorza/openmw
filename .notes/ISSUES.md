# Open issues

- A staged world is not a function of its inputs, because process-global state reaches into it.
  Two pieces of it are left.

- `SceneUtil::Light::mId` counts every light the process has built, and `Rtx::lightPhase` derives a
  flickering light's phase from it — so staging a cell a second time gives every flame a phase the
  first staging did not. `verify --views=arkngthand,arkngthand` renders the first frame identical to
  a run of `--views=arkngthand` and the second `differs: worst 74 of 255 on 54.46% of the pixels`.
  Holding `lightPhase` at a constant makes both frames identical, which is how it was found.

- Staging a cell steps the emitters of shared model templates in place, so a later cell that uses
  the same model starts from the state the earlier staging left. `verify
  --views=balmora,seyda-neen-ship` renders the ship's chimney smoke `differs: worst 26 of 255 on
  0.40% of the pixels` against a run of `--views=seyda-neen-ship` alone, and `--views=vivec,` the
  same. `sadrith-mora` and `dagon-fel` before it change nothing, and those two towns are the ones
  whose architecture carries a different smoke model. It happens with `--props` on and off: a
  chimney is not among the references that switch instances.
