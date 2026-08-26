# Open issues

- An actor's transparency does not reach the traced picture. `MWRender::TransparencyUpdater`
  (`apps/openmw/mwrender/animation.cpp:428`) carries it as two uniforms, `alpha` and `actorFade`,
  and `Rtx::SceneExtractor` reads only the texture-unit uniforms
  (`components/rtx/sceneextractor.cpp:154`). The updater itself is found and applied — `animate`
  handles a `SceneUtil::StateSetUpdater` on a cull callback — so what is lost is the two values
  rather than the callback. Three things ride them: the distance fade over the last tenth of
  `actors processing range`, the Invisibility effect, and Chameleon. All three are fully opaque
  under the ray tracer.

- `Animation::mGlowLight` does not follow the actor it hangs on. It is the light a Light spell or an
  enchanted item puts on an actor (`apps/openmw/mwrender/animation.cpp:1904`), it carries no
  `SceneUtil::LightController`, and its whole output is in its ambient — which `setActorFade` cannot
  reach, since only the controller reads that value and it writes the diffuse and the specular. So
  the glow snaps off at full strength when the actor's node mask cuts at `actors processing range`,
  where every other light an actor carries now fades. Fixing it needs the fade to reach a light's
  ambient, which nothing carries today.
