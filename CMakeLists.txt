cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Set the VITASDK environment variable to your VitaSDK install, e.g. C:/vitasdk")
  endif()
endif()

project(VivoTube C CXX)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "VivoTube")
set(VITA_TITLEID "VIVO00001")
# Keep these two in step: VITA_VERSION is the LiveArea's "XX.YY", VT_VERSION the release tag
# (without the "v") that the auto-updater compares against GitHub's latest release.
set(VITA_VERSION "01.10")
set(VT_VERSION "1.1.0")

# Optional, not checked in: a Google OAuth client to build in (see secrets.cmake.example).
set(VT_GOOGLE_CLIENT_ID "")
set(VT_GOOGLE_CLIENT_SECRET "")
include("${CMAKE_CURRENT_SOURCE_DIR}/secrets.cmake" OPTIONAL)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -Wall")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -Wall -Wno-psabi -Wno-unused-function -fno-strict-aliasing")

file(GLOB CORE_SOURCES src/core/*.cpp)
file(GLOB VITA_SOURCES src/vita/*.cpp)
file(GLOB UI_SOURCES src/ui/*.cpp)

add_executable(VivoTube src/main.cpp ${CORE_SOURCES} ${VITA_SOURCES} ${UI_SOURCES})

target_include_directories(VivoTube PRIVATE src src/core third_party/rapidjson $ENV{VITASDK}/arm-vita-eabi/include/freetype2)
target_compile_definitions(VivoTube PRIVATE VT_VERSION="${VT_VERSION}" VT_VITA=1
  VT_GOOGLE_CLIENT_ID="${VT_GOOGLE_CLIENT_ID}" VT_GOOGLE_CLIENT_SECRET="${VT_GOOGLE_CLIENT_SECRET}")

target_link_libraries(VivoTube
  vita2d
  jpeg
  freetype
  png16
  curl
  mbedtls
  mbedx509
  mbedcrypto
  zstd
  z
  bz2
  SceGxm_stub
  SceDisplay_stub
  SceCtrl_stub
  SceTouch_stub
  SceAudio_stub
  SceAudiodec_stub
  SceVideodec_stub
  SceCodecEngine_stub
  SceSysmodule_stub
  SceNet_stub
  SceNetCtl_stub
  SceAppUtil_stub
  SceAppMgr_stub
  SceCommonDialog_stub
  SceIme_stub
  ScePgf_stub
  ScePower_stub
  SceRtc_stub
  SceSysmem_stub
  SceKernelThreadMgr_stub
  SceKernelModulemgr_stub
  SceIofilemgr_stub
  SceProcessmgr_stub
  SceLibKernel_stub
  # Whole archive: libstdc++ only treats the program as threaded (and only makes std::mutex lock)
  # when pthread_cancel and pthread_cond_* are linked, and nothing references them strongly.
  -Wl,--whole-archive pthread -Wl,--no-whole-archive
  SceKernelThreadMgr_stub
  SceLibKernel_stub
  SceProcessmgr_stub
  stdc++
  m
  c
  gcc
)

vita_create_self(eboot.bin VivoTube UNSAFE)
vita_create_vpk(VivoTube.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  FILE assets/cacert.pem assets/cacert.pem
  FILE assets/clients.json assets/clients.json
  FILE assets/fonts/Roboto-Regular.ttf assets/fonts/Roboto-Regular.ttf
  FILE assets/fonts/Roboto-Medium.ttf assets/fonts/Roboto-Medium.ttf
  FILE assets/fonts/Roboto-Bold.ttf assets/fonts/Roboto-Bold.ttf
  FILE assets/fonts/LICENSE-Roboto.txt assets/fonts/LICENSE-Roboto.txt
)
