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
   :title: validation
   :type: boolean
   :range: true, false
   :default: false

   Load the Vulkan validation layers. A developer setting: with the layers on, any error they report
   stops the process at the call that caused it, so the renderer cannot limp on producing undefined
   contents, and the frame rate is not representative of anything. Off costs nothing.
