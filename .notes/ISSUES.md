# Open issues

- `MWMechanics::Actors` (`apps/openmw/mwmechanics/actors.cpp:1243`) sets an actor's base node mask
  to zero beyond `actors processing range` and back on the frame after, so an actor oscillating
  across that distance takes its carried light in and out of the walk a frame at a time. Measured:
  the range is 7168 units, the largest carried light in the shipped content has a radius of 512 and
  the ordinary torch 256, and a reach of `2 * radius + 128` puts their light 1152 and 640 units
  across. That bubble sits around the actor rather than around the camera, so it is inside the
  picture at 7168 units and does blink. A node mask of zero is excluded by every traversal mask, so
  no mask the mirror chooses can include it.

- An actor's transparency does not reach the traced picture. `MWRender::TransparencyUpdater`
  (`apps/openmw/mwrender/animation.cpp:428`) carries it as two uniforms, `alpha` and `actorFade`,
  and `Rtx::SceneExtractor` reads only the texture-unit uniforms
  (`components/rtx/sceneextractor.cpp:154`). The updater itself is found and applied — `animate`
  handles a `SceneUtil::StateSetUpdater` on a cull callback — so what is lost is the two values
  rather than the callback. Three things ride them: the distance fade over the last tenth of
  `actors processing range`, the Invisibility effect, and Chameleon. All three are fully opaque
  under the ray tracer.
