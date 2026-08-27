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

**What that is not, and the census now says so outright.** 242 of 832 materials on the ship at Seyda
Neen are blend-mode and 356 of 957 in Vivec, which reads like a third of the game. It is not: of
those, the materials whose *own* alpha is under one number **0, 0, 4, 0, 0 and 2** across the ship,
the customs office, Vivec, the canalworks, Wolverine Hall and Balmora. Everything else is foliage,
whose texture alpha is a painted mask that really is binary, and the comment above `alphaPasses`
argues at length that averaging that mask over the ray cone is the better of the two available
errors. That case is served correctly and on purpose.

**So the static world is not the customer.** Two to four surfaces a cell is not what a light-transport
feature is built for. What is built for is the opacity nothing static carries and nothing can count:
an actor's distance fade, Invisibility and Chameleon, which exist only while the game runs. The steps
below are worth doing for those, and the handful of panes come free.

### G1 — an actor's fade, Invisibility and Chameleon reach the surface

Two named uniforms — `alpha` and `actorFade` — read where the texture-unit uniforms already are and
multiplied into the material's opacity, which `Material::isTranslucent` and `GpuMaterial::mOpacity`
already carry to the device. The carrier is new and the destination is not, so this lands after the
transport that gives it somewhere to go — and it is the step the rest were really for. A faded actor
already casts a faded shadow and is already seen through; what is left is telling the surface it is
faded at all.

**Its own negative control**: with the fade working, an actor at nine tenths of `actors processing
range` is already invisible when the mask cuts, which is what upstream built the fade for. The light
that actor carries already fades — `SceneUtil::LightController` multiplies the diffuse by
`LightSource::getActorFade` and the mirror reads that diffuse — so once the geometry follows it, the
cut at `actors processing range` has nothing left to take away.

### G2 — bounces resolve probabilistically, and only when the cost says so

The coin toss, kept where nobody can see it. A translucent candidate on an indirect ray is accepted
or passed through by one draw against its opacity rather than by an ordered walk, which is what makes
the walk above affordable at depth.

**This is the one the tree already argued against, and it was right to.** The comment over
`alphaPasses` rejects per-texel coin tosses because "a canopy comes back as speckle, and it crawls as
the camera moves" — and that is a primary-ray argument. Remix reaches the same conclusion from the
other side: `rtx.enableProbabilisticUnorderedResolveInIndirectRays` is on by default, confined to the
first indirect bounce, "as particles matter less in higher bounces".

**Not until a measurement asks for it.** The eye peels one pane and pays a second trace only on a
pixel that has glass in it, and the census says how few of those there are.

---

## Plan

Each step ends with the build, the filtered test binary, and — where marked — a `shot` or a `bench`
route. No step depends on a later one except where the table says so.

| # | Step | Retires | Risk | Picture |
|---|------|---------|------|---------|
| 1 | G1 — the fade, Invisibility and Chameleon reach the surface | actor transparency | low | **yes** |
| 2 | G2 — bounces resolve probabilistically | — | — | — |

Step 1 is the one the transport was for, and its acceptance test is an actor at a fade rather than a
pane of glass — the census says the static world holds two to four panes a cell. Step 2 is not a fix
until a measurement asks for it.

**Where the shape came from.** Neither shipped path tracer of an old game picks one of the three
answers — both use all of them, split by ray depth: ordered for the eye, approximate for the bounce.
[Q2RTX](https://github.com/NVIDIA/Q2RTX) composites an ordered transparency channel outside its
denoiser; [RTX Remix](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/main/RtxOptions.md) resolves
in order for primary rays and ships a probabilistic unordered path for indirect ones, on by default.
[NVIDIA's ray tracing best practices](https://developer.nvidia.com/blog/best-practices-using-nvidia-rtx-ray-tracing/)
supply the reason to keep the blend out of any-hit: it "interrupts the hardware intersection search",
and hits arrive in an order nothing promises.
