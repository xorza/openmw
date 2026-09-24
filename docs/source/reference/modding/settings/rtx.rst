RTX Settings
############

The experimental Vulkan ray tracing renderer. It replaces primary visibility, shadows, direct and
indirect light, sky, water and fog; the OpenGL renderer is untouched and is what you get with
:code:`enabled = false`.

A build configured with :code:`-DOPENMW_RTX=OFF` leaves it out. It needs an NVIDIA GPU with
hardware ray tracing, Turing or later: acceleration structures, ray query, ray tracing pipelines,
position fetch and the hit objects of the invocation-reorder extension are all required, and a device
missing any of them refuses to start rather than falling back.

Most settings here are read once, at startup. :code:`upscale` and :code:`distant land cells`
also follow the settings window while the game runs.

.. omw-setting::
   :title: enabled
   :type: boolean
   :range: true, false
   :default: false

   Use the ray tracing renderer instead of the OpenGL one. Takes effect on the next start.

.. omw-setting::
   :title: distant land cells
   :type: float32
   :range: ≥ 0
   :default: 4

   How far out from the eye the world is built, in cells. Rays go everywhere, so this says how much
   world exists rather than how far the camera can see, and the fog closes at the same distance —
   air tuned to a shorter reach makes a world built four cells out look like one built none.

   Zero hands the decision back to :code:`viewing distance` in the camera section, which answers a
   different question for a renderer that culls: at 7168 units against a cell of 8192 it barely
   leaves the active grid.

   Takes effect at once: the ring, the air and the map follow the new reach. :code:`object paging`
   and :code:`object paging min size` from the terrain section decide whether and how the distance's
   statics stand, and those two are read once at start.

.. omw-setting::
   :title: upscale
   :type: string
   :range: off, ultraperformance, performance, balanced, quality, dlaa
   :default: quality

   Put DLSS Ray Reconstruction between the trace and the screen. The window's size is what comes
   out; what gets traced is DLSS's answer for it, so anything but :code:`off` is both faster and
   less noisy than tracing at the window's own resolution — Ray Reconstruction reconstructs across
   several frames where the renderer's own filter has one.

   :code:`performance` is the 1920x1080 to 3840x2160 the frame budget is written against.
   :code:`dlaa` denoises and antialiases without upscaling, which is what separates the two halves
   of what it does. :code:`off` traces at the window's size and uses the à-trous filter instead,
   which is what an A/B against the unupscaled path needs.

   Takes effect at once. A mode this machine cannot reach is refused, the renderer keeps the mode
   it had, and the setting is put back to that mode. A name this does not know is refused rather
   than quietly defaulted, and a build without :code:`-DOPENMW_RTX_DLSS=ON` refuses anything but
   :code:`off`.

.. omw-setting::
   :title: preset
   :type: string
   :range: default, d, e
   :default: d

   Which Ray Reconstruction network to run, where :code:`upscale` runs one at all. Ray
   Reconstruction keeps its own presets, and they are not super-resolution's: A through C are
   retired, :code:`d` is the default transformer model and :code:`e` is the latest.

   :code:`default` hands the choice to the installed library. What that picks has changed between
   SDK versions and between the convolutional and transformer models, so two machines under it do
   not run the same network — which is why this is pinned rather than left to it.

   Takes effect on the next start. A name this does not know is refused rather than quietly
   defaulted.

.. omw-setting::
   :title: reflex
   :type: string
   :range: off, on, boost
   :default: on

   NVIDIA Reflex: how the driver paces the frame. Where the driver paces the window at all, the
   game is held before each frame's input is read, so the input is sampled as late as it will
   still reach the screen on time, and the :code:`framerate limit` is enforced at the same point
   rather than after the present. :code:`off` keeps the driver's sleep as a frame limiter and its
   markers as a measurement; :code:`on` asks for the low-latency mode; :code:`boost` holds the
   card at its top clock beside it, at a power cost a laptop's owner decides.

   The driver paces a surface under some present modes and not others — on the driver this was
   built against, under immediate and relaxed FIFO, and under neither mailbox nor FIFO. So with
   :code:`vsync mode` disabled and this on, the window presents immediately rather than through
   mailbox; with it enabled the frame meets the refresh and the driver does not pace it. The
   window's title carries the driver's own input-to-present figure beside the frame rate while
   it paces.

   Takes effect at once. A machine whose driver paces nothing keeps the setting and paces its
   own frames. A name this does not know is refused rather than quietly defaulted.

.. omw-setting::
   :title: reflex flash
   :type: boolean
   :range: true, false
   :default: false

   Mark the frame a left click landed in for the driver's latency analyser, which draws a square
   on that frame. A measurement aid, for a monitor that can time the square against the click,
   and nothing a player wants on.

.. omw-setting::
   :title: specular map layout
   :type: string
   :range: ignore, metal roughness
   :default: ignore

   What the content's :code:`_spec` maps mean. The file cannot say, and two layouts are in use:
   OpenMW's own, with a highlight colour in RGB and glossiness in alpha, and the one of the PBR
   packs, with metalness in red, roughness in green, ambient occlusion in blue and one less
   subsurface scattering in alpha. :code:`ignore` reads no specular map, which is right for the
   first and for content with none. :code:`metal roughness` reads the second.

   The maps are found by name as :ref:`auto use object specular maps` finds them, and loaded with
   the models, so a change requires a restart. A name this does not know is refused rather than
   quietly defaulted.
