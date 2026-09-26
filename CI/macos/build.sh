#!/bin/sh -ex

cd build

if [[ "${MACOS_AMD64}" ]]; then
    arch -x86_64 make -j $(sysctl -n hw.logicalcpu)
else
    make -j $(sysctl -n hw.logicalcpu)
fi
