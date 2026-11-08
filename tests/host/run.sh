#!/bin/sh
# Builds and runs the host tests for src/core with zig's bundled clang.
# Usage (from the VivoTube directory):  sh tests/host/run.sh
set -e
cd "$(dirname "$0")/../.."
mkdir -p build-host
# zig's default cache location can be unwritable from some shells; keep it inside build-host.
export ZIG_GLOBAL_CACHE_DIR="${ZIG_GLOBAL_CACHE_DIR:-$(pwd)/build-host/zig-cache}"
export ZIG_LOCAL_CACHE_DIR="${ZIG_LOCAL_CACHE_DIR:-$(pwd)/build-host/zig-cache-local}"
zig c++ -std=c++17 -O1 -g -Wall -Wno-unused-function \
    -Ithird_party/rapidjson -Isrc/core \
    tests/host/test_main.cpp tests/host/http_stub.cpp \
    src/core/util.cpp src/core/json_util.cpp src/core/parse.cpp src/core/fmp4.cpp \
    src/core/captions.cpp src/core/store.cpp src/core/innertube.cpp \
    src/core/update.cpp src/core/zip.cpp src/core/google.cpp src/core/emoji.cpp src/core/foryou.cpp \
    -o build-host/vivotube_tests.exe
rm -rf tests/host/tmp_store
./build-host/vivotube_tests.exe tests/fixtures/
