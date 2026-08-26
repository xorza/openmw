# Open issues: root causes and a plan

The route, and only what is left of it. A step that is done is **deleted** rather than marked done —
the same rule `ISSUES.md` keeps, and for the same reason: what a finished step knew now lives in the
code that does it, and a plan annotated with its own history stops being a plan.

One entry in `ISSUES.md`, and one cause behind it: **this renderer resolves how opaque a surface is
from one texture's alpha, against a threshold, and from nothing else.** Ordered by what unblocks
what, then by risk. The letters name a group rather than count one, so a gap in them is a group that
is finished.

---

## G. A surface's opacity is one texture's alpha, tested against a threshold

**Retires: the actor's transparency.**

There is exactly one place a hit is confirmed — `alphaPasses` in `rtxvulkan/shaders/lib/traversal.glsl`,
driven by `RTX_RESOLVE_CUTOUTS` — and it reads

    return texel.a >= material.mAlphaCutoff;

That is the whole of it. Three things the content says about opacity never reach that line.

**The material's own alpha.** `NiMaterialProperty` keeps a surface's opacity in its diffuse colour,
and `Surface::Material` says so outright: "Alpha included: `NiMaterialProperty` keeps the surface's
opacity here and nowhere else." The mirror carries it, `SceneBuffers` uploads it, and
`traversal.glsl` reads `material.mDiffuseColour.rgb` — the alpha arrives on the device and is
dropped one line short of being used. `NifOsg::AlphaController` animates that same value, so a
surface the content fades is a surface that does not fade.

**Anything an updater says as a uniform.** The mirror translates OSG *state* — `osg::Material`,
textures, `TexMat`, `BlendFunc` — into a `Surface::Material`, and reads a uniform only where it names
a texture unit (`sceneextractor.cpp:154`). `MWRender::TransparencyUpdater` writes `alpha` and
`actorFade`; `SceneUtil::GlowUpdater` writes `envMapColor`. The updaters themselves *are* found and
run — `SceneExtractor::animate` handles a `StateSetUpdater` on a cull callback — so what is lost is
the values and not the callback. Three things ride those two uniforms: the distance fade over the
last tenth of `actors processing range`, **Invisibility**, and **Chameleon**. All three are fully
opaque under the ray tracer.

**And a partial opacity has nowhere to go anyway.** `Material::getAlphaCutoff` turns
`AlphaMode::Blend` into a cutoff of 0.5, so a blended surface is drawn in or out and never through.

**What that is not.** It is not "a third of the game is wrong", though the census reads that way:
242 of 832 materials on the ship at Seyda Neen are blend-mode, and 356 of 957 in Vivec. Nearly all of
them are foliage, whose texture alpha is a painted mask that really is binary, and the comment above
`alphaPasses` argues at length that averaging that mask over the ray cone is the better of the two
available errors. That case is served correctly and on purpose. What is dropped is the *other*
carrier of opacity — the material's own — which is glass, a ghost, a spell effect, and every actor
the game is trying to fade out.

### G1 — a partially transparent surface stops being a cutout at half

The enabling step, and **it carries the one open design decision in this file.**

`alphaPasses` answers a binary question because traversal asks a binary question. A surface at 0.3
opacity has to become either a hit that is sometimes rejected, or a hit that attenuates what passes
through it. The tree already has an opinion about the first: the comment above `alphaPasses` rejects
per-texel coin tosses for cutouts, because "a canopy comes back as speckle, and it crawls as the
camera moves". Whether that argument carries over to a *uniform* opacity — where the noise is
spatially flat and both the wavelet and Ray Reconstruction are built for exactly that — is the
question to settle before anything is written.

Three shapes, and this file does not pick one:

- **Stochastic.** One random number against the opacity in `alphaPasses`. Cheapest by far, one line,
  and the denoiser is already there. The failure mode is the one the file already names.
- **Transmittance.** The candidate is not rejected; the ray carries a throughput the surface
  multiplies. Correct, and the one that gets stacked glass right. A ray query cannot accumulate
  across candidates without restructuring the loop.
- **A separate pass.** Blended surfaces traced after the opaque hit, in depth order. Most faithful to
  what the content meant and by far the most work.

**Changes the picture**, and it is the only step here that adds a light-transport feature rather than
carrying a number that already exists.

### G2 — the material's alpha reaches the surface it belongs to

`traversal.glsl:254` takes `.rgb` of a `vec4` that has the answer in its fourth component. With G1
landed this is the line that makes glass glass, and it is what `NifOsg::AlphaController` has been
animating into a void.

Small, and it wants the `scene` census extended to count materials whose diffuse alpha is under one —
which is the number that says how much of the game this is about, and nothing prints it today.

### G3 — an actor's fade, Invisibility and Chameleon reach the surface

Two named uniforms, read where the texture-unit uniforms already are, multiplied into the material's
alpha. It is the same statement as G2 arriving by a different carrier, so it lands after it.

**Its own negative control**: with the fade working, an actor at nine tenths of `actors processing
range` is already invisible when the mask cuts, which is what upstream built the fade for. The light
that actor carries already fades — `SceneUtil::LightController` multiplies the diffuse by
`LightSource::getActorFade` and the mirror reads that diffuse — so once the geometry follows it, the
cut at `actors processing range` has nothing left to take away.

---

## Plan

Each step ends with the build, the filtered test binary, and — where marked — a `shot` or a `bench`
route. No step depends on a later one except where the table says so.

| # | Step | Retires | Risk | Picture |
|---|------|---------|------|---------|
| 1 | G1 — a partial opacity is traced as one | — | **high, and a decision first** | **yes, large** |
| 2 | G2 — the material's alpha reaches the surface | — | low, after 1 | **yes** |
| 3 | G3 — the fade, Invisibility and Chameleon reach it | actor transparency | low, after 2 | **yes** |

Step 1 is the only real feature in this file and the only one that needs a decision before a line is
written; steps 2 and 3 are numbers that already exist arriving where they were always going, and
neither looks right until 1 has landed.
