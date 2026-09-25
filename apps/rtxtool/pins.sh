# Every download `rtx` pins, by version and SHA-256, sourced by it: the one file a bump edits, and
# what CI keys its cache of deps/ on beside upstream's `CI/deps_versions.msvc.sh`, so a bump saves
# the cache afresh and an edit to the script around the pins does not. Why each is pinned the way
# it is stays with the function that fetches it.
# shellcheck shell=bash disable=SC2034

# SDL's own Windows package, `fetchWindowsSdl`. Exported, because the Windows preset names its
# directory through it.
export RTX_SDL2_VERSION="2.32.10"
sdl2Sha256="af347939395a58b365846aaea27391e69f9ec9d4dd650d6ac40802159b418a6e"

# aqt, which installs Qt for the package flavour on Windows, `fetchWindowsQt`.
aqtRelease="v3.1.15"
aqtSha256="f9e9acc05975f2e70e4935ace76b41882cda4b53eb779c9bcf1d87a95029a6b0"

# LLVM's Windows package, for the clang-format CI pins, `fetchClangFormat`.
llvmRelease="14.0.6"
llvmSha256="e8dbb2f7de8e37915273d65c1c2f2d96844b96bb8e8035f62c5182475e80b9fc"

# The AppImage tools, `fetchAppImageTools`: a name, where it comes from and its checksum, a line each.
appImageTools="\
linuxdeploy https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
linuxdeploy-plugin-qt https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-qt-x86_64.AppImage 15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724
linuxdeploy-plugin-appimage https://github.com/linuxdeploy/linuxdeploy-plugin-appimage/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-appimage-x86_64.AppImage 992d502a248e14ab185448ddf6f6e7d25558cb84d4623c354c3af350c25fccb3
runtime-x86_64 https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64 2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d"
