OpenMW RTX
==========

A fork of [OpenMW](https://openmw.org) that makes Morrowind work your GPU hard again:
ray-traced lighting, path-traced indirect light and DLSS upscaling.

This tree is OpenMW 0.52 plus one renderer. Everything about the engine itself — what it is,
how to install it, how to build it, the data path, the command line — is in the
[upstream README](https://gitlab.com/OpenMW/openmw/-/blob/master/README.md). This file covers
only what the fork adds.

Screenshots
-----------

<!-- screenshots -->

Demo video
----------

<!-- demo video -->

What the fork is
----------------

Upstream OpenMW stays the host engine: cells, references, physics, scripts, animation, weather
and the GUI. It no longer owns the picture. A second renderer stands beside the OpenGL rasterizer
and replaces the whole image: primary visibility, shadows, direct and indirect light, sky, water
and fog are ray traced on the GPU. The rasterizer is not modified. One binary ships both
renderers, and the one not chosen never starts.

Vanilla content is read as it is. Morrowind's textures are pre-lit, so the renderer estimates
the painted light and divides it out to recover materials the new light transport can use.

Goal
----

A 2002 game made to look astonishing on current hardware. Vanilla content, new light transport.

Requirements
------------

* NVIDIA RTX, Turing (RTX 20 series) or later
* Vulkan 1.4 with ray tracing pipelines, ray queries, position fetch and shader invocation
  reorder. A device missing any of them refuses to start rather than falling back.
* DLSS Ray Reconstruction as the denoiser and upscaler (NGX, on by default at build time)

Building and running
--------------------

The renderer is built by default. `-DOPENMW_RTX=OFF` leaves it out. `OPENMW_RTX_DLSS` (default
`ON`) links NGX and needs `NGX_ROOT`. Turn the renderer on with `[RTX] enabled = true` in
`settings.cfg`.

* [Architecture](docs/rtx/architecture.md) — the seam, the layers, who owns whom, the order a
  frame is computed in
* [Settings](docs/source/reference/modding/settings/rtx.rst) — every `[RTX]` setting

License
-------

GPLv3, as upstream. See [LICENSE](LICENSE).
