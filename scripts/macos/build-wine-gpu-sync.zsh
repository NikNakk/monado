#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
# Builds only the test producer, using the existing Wine/DXMT checkout.
set -euo pipefail
repo=${0:A:h:h:h}
: ${GPU_SYNC_WINE_BUILD:?test artifact directory outside the repository}
: ${GPU_SYNC_WINE_RUNTIME:?Wine runtime with bin/wine and bin/winebuild}
: ${GPU_SYNC_WINE_SOURCE:?matching Wine source directory}
: ${GPU_SYNC_DXMT_SOURCE:?DXMT checkout with IDXMTNativeDevice2}
root=${GPU_SYNC_WINE_BUILD}
mkdir -p ${root}/{support/wine,x86_64-windows,x86_64-unix}
for header in debug.h list.h unixlib.h; do
    ln -sf ${GPU_SYNC_WINE_SOURCE}/include/wine/${header} ${root}/support/wine/${header}
done
x86_64-w64-mingw32-g++ -c -std=c++17 -O2 -fno-exceptions -fno-rtti \
    -D__WINESRC__ -D_WIN64 -DWINE_NO_LONG_TYPES -I${root}/support \
    -I${GPU_SYNC_DXMT_SOURCE}/include -I${GPU_SYNC_DXMT_SOURCE}/include/native/directx \
    ${repo}/tests/macos_gpu_sync_wine_pe.cpp -o ${root}/producer.o
libs=${GPU_SYNC_WINE_RUNTIME}/lib/wine/x86_64-windows
x86_64-w64-mingw32-g++ -shared ${root}/producer.o ${libs}/libwinecrt0.a ${libs}/libntdll.a \
    -static -lkernel32 -ldxguid -Wl,--export-all-symbols \
    -o ${root}/x86_64-windows/monado_gpu_sync.dll
${GPU_SYNC_WINE_RUNTIME}/bin/winebuild --builtin ${root}/x86_64-windows/monado_gpu_sync.dll
clang++ -arch x86_64 -std=c++17 -shared -fblocks ${repo}/tests/macos_gpu_sync_wine_native.mm \
    -framework Foundation -framework Metal -framework IOSurface \
    -o ${root}/x86_64-unix/monado_gpu_sync.so
x86_64-w64-mingw32-g++ -std=c++17 -O2 -static ${repo}/tests/macos_gpu_sync_wine_main.cpp \
    -ld3d11 -o ${root}/producer.exe
file ${root}/producer.exe ${root}/x86_64-unix/monado_gpu_sync.so

# Native DLL search starts beside the executable; use the matched runtime set.
cp ${root}/x86_64-windows/monado_gpu_sync.dll ${root}/
for dll in d3d11.dll dxgi.dll winemetal.dll; do
    cp ${libs}/${dll} ${root}/
done
