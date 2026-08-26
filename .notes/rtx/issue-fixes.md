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

### G1 — the material's alpha reaches the material

**First, and not because it is small.** It is what tells a translucent surface from a painted mask,
and every step below needs that answer before it can act.

`traversal.glsl:254` takes `.rgb` of a `vec4` that has the opacity in its fourth component. The
mirror fills it, `SceneBuffers` uploads it, and the shader reads three quarters of it — so the number
`NifOsg::AlphaController` animates arrives on the device and is dropped one line short.

Nothing changes on screen until G2. What changes is that traversal can finally distinguish the two
populations that `AlphaMode::Blend` currently lumps together: a leaf card, whose material is fully
opaque and whose texture carries a painted binary mask, and a pane of glass, whose material says 0.3.
Without that, giving a blended surface transmittance turns every leaf in Vvardenfell into gauze.

Wants the `scene` census extended to count materials whose diffuse alpha is under one, which is the
number that says how much of the game this is about and which nothing prints today.

### G2 — a blended surface casts a blended shadow

`occluded` stops committing a candidate whose material is translucent and multiplies a throughput
instead. Exact, ordered, free of noise, and the cheapest of the three: a shadow ray already runs to
completion through the same macro, and nothing about the answer depends on which candidate arrived
first.

**The precedent is direct.** RTX Remix ships `rtx.enableDirectAlphaBlendShadows` and
`rtx.enableIndirectAlphaBlendShadows` on by default, and keeps coloured translucent shadows behind a
separate option that is off. So: attenuate first, tint later or never.

A half-opaque pane halves the light under it. A cutout keeps the branch it has today.

### G3 — the camera walks the blended layers in order

`traceSurface` keeps the nearest few translucent candidates by distance and composites them over the
opaque hit it commits. Noise-free, and this is where Morrowind's glass, ghosts and faded actors
actually are.

**Both shipped analogues do exactly this for the eye.** Q2RTX collects transparent surfaces between
the camera and the primary hit into a transparency channel of its own, ordered by distance, and
composites afterwards — outside the denoised path. Remix resolves them in order for primary rays and
keeps its approximations for bounces.

**Not an any-hit blend.** Hits arrive in an undefined order, which is the whole reason the ordered
walk has to hold the layers and sort them rather than blend as it goes. NVIDIA's own guidance is that
an any-hit shader "interrupts the hardware intersection search" and wants to stay unified and simple,
which is an argument for keeping the blend out of it.

A fixed, small layer budget. Past it the furthest layer is dropped, which is what a budget is for.

**Changes the picture**, and it is the step that makes glass glass.

### G4 — an actor's fade, Invisibility and Chameleon reach the surface

Two named uniforms — `alpha` and `actorFade` — read where the texture-unit uniforms already are and
multiplied into the material's opacity. The same statement as G1 arriving by a different carrier, so
it lands after the transport that gives it somewhere to go.

**Its own negative control**: with the fade working, an actor at nine tenths of `actors processing
range` is already invisible when the mask cuts, which is what upstream built the fade for. The light
that actor carries already fades — `SceneUtil::LightController` multiplies the diffuse by
`LightSource::getActorFade` and the mirror reads that diffuse — so once the geometry follows it, the
cut at `actors processing range` has nothing left to take away.

### G5 — bounces resolve probabilistically, and only when the cost says so

The coin toss, kept where nobody can see it. A translucent candidate on an indirect ray is accepted
or passed through by one draw against its opacity rather than by an ordered walk, which is what makes
the walk above affordable at depth.

**This is the one the tree already argued against, and it was right to.** The comment over
`alphaPasses` rejects per-texel coin tosses because "a canopy comes back as speckle, and it crawls as
the camera moves" — and that is a primary-ray argument. Remix reaches the same conclusion from the
other side: `rtx.enableProbabilisticUnorderedResolveInIndirectRays` is on by default, confined to the
first indirect bounce, "as particles matter less in higher bounces".

**Not until a measurement asks for it.** G3 may be affordable at every depth in a world this small.

---

## Plan

Each step ends with the build, the filtered test binary, and — where marked — a `shot` or a `bench`
route. No step depends on a later one except where the table says so.

| # | Step | Retires | Risk | Picture |
|---|------|---------|------|---------|
| 1 | G1 — the material's alpha reaches the material | — | none | no, on its own |
| 2 | G2 — a blended surface casts a blended shadow | — | low | **yes** |
| 3 | G3 — the camera walks the blended layers in order | — | medium | **yes, large** |
| 4 | G4 — the fade, Invisibility and Chameleon reach it | actor transparency | low, after 3 | **yes** |
| 5 | G5 — bounces resolve probabilistically | — | — | — |

Step 1 changes nothing on screen and unblocks everything: without it a leaf card and a pane of glass
are the same thing to traversal. Steps 2 and 3 are the light transport, cheapest first. Step 4 is a
number that already exists arriving where it was always going. Step 5 is not a fix until a
measurement asks for it.

**Where the shape came from.** Neither shipped path tracer of an old game picks one of the three
answers — both use all of them, split by ray depth: ordered for the eye, approximate for the bounce.
[Q2RTX](https://github.com/NVIDIA/Q2RTX) composites an ordered transparency channel outside its
denoiser; [RTX Remix](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/main/RtxOptions.md) resolves
in order for primary rays and ships a probabilistic unordered path for indirect ones, on by default.
[NVIDIA's ray tracing best practices](https://developer.nvidia.com/blog/best-practices-using-nvidia-rtx-ray-tracing/)
supply the reason to keep the blend out of any-hit: it "interrupts the hardware intersection search",
and hits arrive in an order nothing promises.
