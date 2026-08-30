RTX Settings
############

The experimental Vulkan ray tracing renderer. It replaces primary visibility, shadows, direct and
indirect light, sky, water and fog; the OpenGL renderer is untouched and is what you get with
:code:`enabled = false`.

It exists only in a build configured with :code:`-DOPENMW_RTX=ON`, and it needs an Ada-class NVIDIA
GPU: acceleration structures, ray query, ray tracing pipelines, position fetch, opacity micromaps and
shader execution reordering are all required, and a device missing any of them refuses to start
rather than falling back.

Every setting here is read once, at startup.

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

.. omw-setting::
   :title: upscale
   :type: string
   :range: off, performance, balanced, quality, dlaa
   :default: quality

   Put DLSS Ray Reconstruction between the trace and the screen. The window's size is what comes
   out; what gets traced is DLSS's answer for it, so anything but :code:`off` is both faster and
   less noisy than tracing at the window's own resolution — Ray Reconstruction reconstructs across
   several frames where the renderer's own filter has one.

   :code:`performance` is the 1920x1080 to 3840x2160 the frame budget is written against.
   :code:`dlaa` denoises and antialiases without upscaling, which is what separates the two halves
   of what it does. :code:`off` traces at the window's size and uses the à-trous filter instead,
   which is what an A/B against the unupscaled path needs.

   A name this does not know is refused rather than quietly defaulted, and a build without
   :code:`-DOPENMW_RTX_DLSS=ON` refuses anything but :code:`off`.

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

   A name this does not know is refused rather than quietly defaulted.
