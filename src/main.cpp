// VivoTube entry point: system module and network setup, then the app loop.
#include <psp2/apputil.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <vita2d.h>

#include <cerrno>
#include <cstdlib>
#include <string>

#include "core/http.h"
#include "core/util.h"
#include "ui/app.h"
#include "ui/gfx.h"

#ifndef VT_VERSION
#define VT_VERSION "1.0"
#endif

extern "C" {
// newlib heap for malloc/new: JSON documents, segment buffers, decoded thumbnails.
int _newlib_heap_size_user = 128 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 1024 * 1024;

// newlib's generic getentropy() (pulled in by mbedTLS) expects this hook; back it with the
// kernel's random number generator, which returns at most 64 bytes per call.
struct _reent;
int _getentropy_r(struct _reent*, void* buf, size_t len) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    while (len > 0) {
        size_t n = len > 64 ? 64 : len;
        if (sceKernelGetRandomNumber(p, n) < 0) {
            errno = EIO;
            return -1;
        }
        p += n;
        len -= n;
    }
    return 0;
}
}

static const char* kDataDir = "ux0:data/VivoTube";

static void net_init() {
    int r = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    if (r < 0) VT_LOG("load NET module: 0x%08X", (unsigned)r);
    static const int kNetMem = 1024 * 1024;
    SceNetInitParam p;
    p.memory = malloc(kNetMem);
    p.size = kNetMem;
    p.flags = 0;
    r = sceNetInit(&p);
    if (r < 0) VT_LOG("sceNetInit: 0x%08X", (unsigned)r);
    r = sceNetCtlInit();
    if (r < 0) VT_LOG("sceNetCtlInit: 0x%08X", (unsigned)r);
}

int main() {
    using namespace vt;
    make_dirs(kDataDir);
    log_open(std::string(kDataDir) + "/log.txt");
    VT_LOG("VivoTube %s starting", VT_VERSION);

    SceAppUtilInitParam initParam{};
    SceAppUtilBootParam bootParam{};
    sceAppUtilInit(&initParam, &bootParam);
    sceSysmoduleLoadModule(SCE_SYSMODULE_PGF);
    int r = sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC);
    if (r < 0) VT_LOG("load AVCDEC module: 0x%08X", (unsigned)r);
    net_init();

    // A larger temporary pool than the 1 MB default: every text glyph and rectangle drawn in a
    // frame takes vertex memory from it.
    vita2d_init_advanced(8 * 1024 * 1024);
    vita2d_set_vblank_wait(1);
    if (!g_gfx.init()) VT_LOG("could not load the system font");

    if (!http_init("app0:assets/cacert.pem")) VT_LOG("http init failed");
    {
        std::string flag;
        if (read_file(std::string(kDataDir) + "/debug_http", flag)) {
            http_set_debug(true);
            VT_LOG("http debug logging on");
        }
    }

    App* app = new App();
    g_app = app;
    app->init(kDataDir);
    app->run();
    app->shutdown();
    delete app;
    g_app = nullptr;

    http_shutdown();
    g_gfx.shutdown();
    vita2d_fini();
    sceNetCtlTerm();
    sceNetTerm();
    sceKernelExitProcess(0);
    return 0;
}
