# Open issues

- `Animation::mGlowLight` does not follow the actor it hangs on. It is the light a Light spell or an
  enchanted item puts on an actor (`apps/openmw/mwrender/animation.cpp:1904`), it carries no
  `SceneUtil::LightController`, and its whole output is in its ambient — which `setActorFade` cannot
  reach, since only the controller reads that value and it writes the diffuse and the specular. So
  the glow snaps off at full strength when the actor's node mask cuts at `actors processing range`,
  where every other light an actor carries now fades. Fixing it needs the fade to reach a light's
  ambient, which nothing carries today.
