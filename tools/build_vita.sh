#!/bin/sh
# Builds VivoTube.vpk. Run from any shell that has VitaSDK's MSYS-style paths, e.g. on Windows:
#   C:\devkitPro\msys2\usr\bin\bash.exe -l /c/path/to/VivoTube/tools/build_vita.sh
# Set VITASDK first if it is not /c/vitasdk (Windows) or /usr/local/vitasdk (Linux/macOS).
set -e
cd "$(dirname "$0")/.."
if [ -z "$VITASDK" ]; then
    if [ -d /c/vitasdk ]; then VITASDK=/c/vitasdk; else VITASDK=/usr/local/vitasdk; fi
fi
export VITASDK
export PATH="$VITASDK/bin:$PATH"
mkdir -p build
cd build
if [ "$1" = "clean" ] || ! grep -q "arm-vita-eabi" CMakeCache.txt 2>/dev/null; then
    rm -rf CMakeCache.txt CMakeFiles
fi
cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release ..
make ${VT_MAKE_FLAGS:--j4}
ls -l VivoTube.vpk
