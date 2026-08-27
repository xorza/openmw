# Open issues: root causes and a plan

The route, and only what is left of it. A step that is done is **deleted** rather than marked done —
the same rule `ISSUES.md` keeps, and for the same reason: what a finished step knew now lives in the
code that does it, and a plan annotated with its own history stops being a plan.

Nothing in `ISSUES.md` waits on this file any more. Two steps are left. Neither is due yet, and the
second is an optimisation rather than a fix.

---

## G3 — the eye walks the see-through surfaces in order rather than peeling one

`visibility.comp` peels the nearest see-through surface and traces on from it, which is exact for
glass and is not exact for a person. The census says a cell holds two to four translucent materials,
so one along a ray is nearly always all there is — but an actor is a cuirass over a skirt over a leg,
and a fade applies to all three at once. Under Chameleon the nearest layer fades and the one behind
it stands at full strength.

**What was bought by not doing it.** No sorting, no layer budget, and no second traversal on a pixel
with nothing see-through in it. The transport that reads the fade landed first because it is what
makes the question askable at all, and one layer is what the eye was given *to begin with*.

**Its own negative control**: a person under Chameleon at arm's length, which is where the layers are
largest and the error is easiest to see. Glass is not the test for this one.

---

## G2 — bounces resolve probabilistically, and only when the cost says so

The coin toss, kept where nobody can see it. A see-through candidate on an indirect ray is accepted
or passed through by one draw against its opacity, rather than by the ordered walk the eye does —
which is what would make an ordered walk affordable at depth if one were ever wanted there.

**This is the one the tree already argued against, and it was right to.** The comment over
`alphaPasses` rejects per-texel coin tosses because "a canopy comes back as speckle, and it crawls as
the camera moves" — and that is a primary-ray argument. Remix reaches the same conclusion from the
other side: `rtx.enableProbabilisticUnorderedResolveInIndirectRays` is on by default, confined to the
first indirect bounce, "as particles matter less in higher bounces".

**Not until a measurement asks for it.** The eye peels one surface and pays a second trace only on a
pixel that has something see-through in it. The census says a cell holds two to four translucent
materials, and an actor is see-through only while the game is fading it.

**Where the shape came from.** Neither shipped path tracer of an old game picks one of the three
answers — both use all of them, split by ray depth: ordered for the eye, approximate for the bounce.
[Q2RTX](https://github.com/NVIDIA/Q2RTX) composites an ordered transparency channel outside its
denoiser; [RTX Remix](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/main/RtxOptions.md) resolves
in order for primary rays and ships a probabilistic unordered path for indirect ones, on by default.
[NVIDIA's ray tracing best practices](https://developer.nvidia.com/blog/best-practices-using-nvidia-rtx-ray-tracing/)
supply the reason to keep the blend out of any-hit: it "interrupts the hardware intersection search",
and hits arrive in an order nothing promises.
