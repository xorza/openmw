# CI: the ray tracer on Ubuntu, upstream's workflows kept and switched off

Design and plan for the first CI: one workflow of the fork's own that builds and tests the
ray-tracing build on Ubuntu, beside upstream's four workflow files left byte-for-byte as they
are. Windows comes after a local build on a Windows box, and a release comes after that; the
findings for both are at the end. Delete a step when it lands.

## Findings that shape the design

- **Upstream's workflows have never run on the fork.** All four are `active` and there are zero
  runs, which is the fork gate GitHub puts on Actions until somebody clicks through it. Once
  clicked, `push.yml` would run on every push and build the rasterizer-only tree: verified
  locally today in `build-nortx/` (1231 steps; 532 + 154 + 1582 tests pass).
- **A workflow is switched off in the repository, not in its file.** `gh workflow disable
  push.yml` sets its state to `disabled_manually`, which the REST API lists beside `active`,
  `disabled_fork` and `disabled_inactivity`, and only `gh workflow enable` undoes. The state
  lives in the repository's settings, so the file stays byte-for-byte upstream's. `windows.yml`
  and `macos.yml` have no trigger of their own (`workflow_call`), so there is nothing to switch
  off in them. This is the zero-diff answer to "left as is but disabled".
- **The distro's Vulkan is too old, and so is LunarG's apt repository.** The backend requires
  `VK_EXT_ray_tracing_invocation_reorder` and the shaders `GL_EXT_shader_invocation_reorder`,
  both of which entered the SDK at 1.4.333. Ubuntu 24.04 ships headers 1.3.275, and LunarG
  stopped updating its Ubuntu packages after May 2025 at 1.4.313. What works is LunarG's Linux
  tarball: `vulkansdk-linux-x86_64-<version>.tar.xz` from `sdk.lunarg.com`, 330 MB, with a
  published SHA-256. This box builds against 1.4.357, and 1.4.357.1 is the latest tarball.
- **The NGX SDK is a public repository, so `actions/checkout` fetches it.** A second checkout
  step with `repository: NVIDIA/DLSS`, `ref: v310.7.0`, `path: dlss` and a `sparse-checkout` of
  `include` and `lib/Linux_x86_64/rel` fetches about 150 MB: cone mode brings the files of the
  parent directories along, which is where `libnvsdk_ngx.a` sits, and leaves `dev/` and the
  Windows libraries behind. `FindNGX.cmake` then reads `NGX_ROOT`.
- **No GPU on a hosted runner.** Every RTX test that needs a device skips at `vkCreateInstance`
  (no ICD), and `check`, `repeat` and `bench` cannot run. What CI proves is that both binaries
  compile, that the format check holds, and that the device-free tests pass: the source-tree
  test, the digests, the sheets, the sky, the readers. The gate stays on this box. GitHub's own
  hardening guide says a self-hosted runner "should almost never be used for public
  repositories", so a GPU runner is not in this plan.
- **`rtx.sh` is the one grammar, and CI can speak it.** `rtx.sh debug build` and `rtx.sh debug
  test` are what the desk runs; the workflow runs the same two lines, so CI and the desk cannot
  configure differently. The script needs two small additions, below.
- **Hardening, from GitHub's guide.** Pin every action to a full commit SHA (the only immutable
  reference), give the token `contents: read` and nothing more, and cancel superseded runs
  with a `concurrency` group. Upstream pins to major tags; the fork's workflow pins to SHAs.

## Design

Three things, and nothing in upstream's files:

1. **Upstream's `push.yml` and `release.yml` switched off** with two commands, run once by the
   repository's owner and recorded in AGENTS.md so a fresh fork repeats them:
   `gh workflow disable push.yml` and `gh workflow disable release.yml`.
2. **`.github/workflows/rtx.yml`**, a new file. Two jobs on `ubuntu-24.04`:
   - `format`: `clang-format-14` from apt, then `CLANG_FORMAT=clang-format-14
     CI/check_clang_format.sh`. Two minutes, fails fast, upstream's own script.
   - `ubuntu-rtx`: OpenMW's dependencies from the PPA through upstream's
     `CI/install_debian_deps.sh`, plus `ninja-build`, `libgtest-dev` and `libgmock-dev` because
     `rtx.sh` asks for Ninja and the system Google Test; the Vulkan SDK tarball, checksum
     verified, extracted to the four things the build needs and cached under its version; the
     NGX checkout; ccache; then `rtx.sh debug build`, `rtx.sh debug test`, and
     `rtx.sh nodlss build openmw-rtx-vulkan`. Logs and `CMakeCache.txt` uploaded on failure.
3. **Two additions to `rtx.sh`**: `build` takes explicit targets after the verb, so CI can
   compile the backend alone in the `nodlss` flavour the way the gate does; and `runTests`
   prints gtest's `[  SKIPPED ]` line beside `[  PASSED  ]`, so a log where every device test
   skipped says so.

The workflow is `.github/workflows/rtx.yml`.

What each choice buys:

- `ubuntu-24.04` by name and not `ubuntu-latest`, so the image moves when the file says so.
- The SDK tarball is verified against LunarG's published SHA-256 before anything is extracted,
  and only the headers, the loader, `glslc` and `spirv-val` are extracted: the cache holds
  about 100 MB instead of a gigabyte, and the layers are not needed where no device exists.
  `FindVulkan` reads `VULKAN_SDK`; `LD_LIBRARY_PATH` is what lets `components-tests` start
  and skip.
- `NGX_ROOT` in the environment is exactly how `FindNGX.cmake` is pointed at the checkout on
  the desk, and `rtx.sh` passes nothing about it — one door.
- `rtx.sh` configures with ccache and mold already, so the ccache action needs only to put a
  warm cache on the path. Its key is per runner image and flavour.
- A pull request from anybody runs on GitHub's ephemeral machine with a read-only token, which
  is what the hardening guide asks of a public repository.

## Plan

Steps 1 and 2 have landed: `rtx.sh` takes explicit build targets and prints the skip line, and
`.github/workflows/rtx.yml` is in the tree with the AGENTS.md paragraph beside it.

**Step 3 — push to a branch and watch the first run.** Open a pull request or run `Ray tracing`
from the Actions tab on the branch. Expect up to an hour cold and ten to fifteen minutes warm.
Iterate there, not on `master`.

**Step 4 — switch upstream's off, then merge.** `gh workflow disable push.yml` and
`gh workflow disable release.yml`, run by you, before the merge lands on `master`; otherwise the
merge runs `push.yml` once. Verified in the Actions tab: `push.yml` shows as disabled, `Ray
tracing` ran on the merge.

**Step 5 — read the first green run.** The test step's skip count is what a machine with no
device shows; this box shows none. Both numbers are in the log.

## Later: Windows, then a release

Not in this plan; kept so the findings are not lost. Windows waits for a local build on the
Windows box first.

- Known non-portable spots: `FindNGX.cmake` (Windows keeps `lib/Windows_x86_64/x64/nvsdk_ngx_d.lib`
  and `lib/Windows_x86_64/rel/nvngx_dlssd.dll`, whose version is the UTF-16 string
  `310,7,0,0` in its resource, readable with `file(STRINGS ... ENCODING UTF-16LE)`); perf's
  fifo in `components/rtxbench/frametimes.cpp`; `popen` in `gpuclock.cpp`; `__builtin_trap` in
  `components/rtx/contract.hpp`. MSVC will find more.
- A release needs the feature library beside the binary: `dlss.cpp` hands NGX a compile-time
  absolute path today, and the backend loads Ray Reconstruction only, so what ships is
  `libnvidia-ngx-dlssd.so.310.7.0` or `nvngx_dlssd.dll`, 41 MB. Linux wants an AppImage from
  `ubuntu-22.04`; Windows has upstream's NSIS package in `windows.yml`; the fork's tags want
  their own pattern.
- Before the first public binary with DLSS in it: OpenMW is GPLv3, NVIDIA's licence §1.c
  permits distribution inside an application and §4.e forbids use that would subject the SDK
  to an open source licence. The `nodlss` build is clean to ship; the DLSS build is a decision.
