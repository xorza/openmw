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

<img width="2560" height="1366" alt="screenshot017" src="https://github.com/user-attachments/assets/d99644a6-2395-4a9b-a095-53a54c7a27b2" />
<img width="2560" height="1366" alt="screenshot016" src="https://github.com/user-attachments/assets/cc181eb8-713c-41f2-a093-2aea60d71c2a" />
<img width="2560" height="1366" alt="screenshot015" src="https://github.com/user-attachments/assets/3490ca2f-df43-4850-b619-d63b28720f82" />
<img width="2560" height="1366" alt="screenshot014" src="https://github.com/user-attachments/assets/7d53a672-1eb9-404c-9ad2-107ab6a102b8" />
<img width="2560" height="1366" alt="screenshot008" src="https://github.com/user-attachments/assets/ad180077-1a39-4921-ba38-fc34aa566e55" />
<img width="2560" height="1366" alt="screenshot003" src="https://github.com/user-attachments/assets/7ecbd7a4-37f5-4761-8596-79a2870a3a75" />
<img width="2560" height="1366" alt="screenshot001" src="https://github.com/user-attachments/assets/2a5855df-bc04-4b08-929a-51edee2c4fcc" />
<img width="2560" height="1366" alt="screenshot000" src="https://github.com/user-attachments/assets/d85dd6c8-8a77-4ddc-ae83-6ac1f58d42fb" />


Demo video
----------

[Watch the demo on YouTube](https://www.youtube.com/watch?v=h9wsxzmaoqM)

[![OpenMW RTX demo](https://img.youtube.com/vi/h9wsxzmaoqM/maxresdefault.jpg)](https://www.youtube.com/watch?v=h9wsxzmaoqM)

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

* NVIDIA only: an RTX card, Turing (RTX 20 series) or later. The renderer is built on NVIDIA's
  extensions and on DLSS, so no AMD or Intel GPU runs it.
* Vulkan 1.4 with ray tracing pipelines, ray queries, position fetch and shader invocation
  reorder. A device missing any of them refuses to start rather than falling back.
* NVIDIA driver 595 or later, on Windows or Linux. It is the first to offer
  `VK_EXT_ray_tracing_invocation_reorder` on RTX cards, Turing included, so an RTX 20 card on an
  older driver is refused for that extension and runs once the driver is updated.
* DLSS Ray Reconstruction as the denoiser and upscaler (NGX, on by default at build time)

Tested on one machine so far: a laptop RTX 4090 at 150 W, which is about a desktop RTX 4070.

Building and running
--------------------

The renderer is built by default. `-DOPENMW_RTX=OFF` leaves it out. `OPENMW_RTX_DLSS` (default
`ON`) links NGX and needs `NGX_ROOT`. Turn the renderer on with `[RTX] enabled = true` in
`settings.cfg`.

* [Architecture](docs/rtx/architecture.md) — the seam, the layers, who owns whom, the order a
  frame is computed in
* [Settings](docs/source/reference/modding/settings/rtx.rst) — every `[RTX]` setting

When it crashes
---------------

The game writes a report of every crash, and of every hang longer than 20 seconds. Once the game
is gone, the log and every dump of the session go into one file, `OpenMW-crash-<time>.zip` under
`crashes` in the user data folder, and a dialog names it. Attach that file to a
[new issue](https://github.com/xorza/openmw-rtx/issues). Inside it:

* `openmw.log`. Its last lines, which begin `Crash:` or `Hang:`, say what happened and what the
  game was doing.
* The dump: every thread's stack.

Each release publishes its symbols, `-symbols.zip`, beside its archive, and
`rtx package crash <dump> <symbols>` reads a dump against them. `[General] crash hang seconds`
sets the hang limit, and `OPENMW_DISABLE_CRASH_CATCHER=1` turns the catcher off.

License
-------

GPLv3, as upstream. See [LICENSE](LICENSE).
