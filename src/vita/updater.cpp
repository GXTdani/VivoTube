#include "updater.h"

#include <psp2/appmgr.h>
#include <psp2/io/devctl.h>
#include <psp2/io/fcntl.h>
#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "../core/util.h"
#include "../core/zip.h"

namespace vt {

namespace {

bool inflate_raw(const uint8_t* src, size_t srcLen, std::string& out, size_t outLen, std::string* err) {
    out.assign(outLen, '\0');
    z_stream zs{};
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
        if (err) *err = "zlib init failed";
        return false;
    }
    zs.next_in = const_cast<Bytef*>(src);
    zs.avail_in = (uInt)srcLen;
    zs.next_out = reinterpret_cast<Bytef*>(&out[0]);
    zs.avail_out = (uInt)outLen;
    int r = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    if (r != Z_STREAM_END || zs.total_out != outLen) {
        if (err) *err = "corrupt data in the update";
        return false;
    }
    return true;
}

// Returns 0 or the system's error code, so failures can say why.
int write_all(const std::string& path, const std::string& data) {
    SceUID fd = sceIoOpen(path.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) return fd;
    size_t off = 0;
    int err = 0;
    while (off < data.size()) {
        int n = sceIoWrite(fd, data.data() + off, (SceSize)std::min<size_t>(data.size() - off, 1u << 20));
        if (n <= 0) {
            err = n < 0 ? n : -1;
            break;
        }
        off += (size_t)n;
    }
    int c = sceIoClose(fd);
    return err ? err : (c < 0 ? c : 0);
}

std::string parent_dir(const std::string& p) {
    size_t s = p.rfind('/');
    return s == std::string::npos ? std::string() : p.substr(0, s);
}

}  // namespace

bool install_update(const std::string& vpk, const std::string& appDir, std::string* err, std::atomic<int>* done,
                    std::atomic<int>* total) {
    std::vector<ZipEntry> entries;
    if (!zip_entries(vpk, entries, err)) return false;
    std::vector<const ZipEntry*> files;
    bool hasEboot = false;
    for (const ZipEntry& e : entries) {
        if (e.isDir() || starts_with(e.name, "sce_sys/")) continue;
        if (e.name == "eboot.bin") hasEboot = true;
        files.push_back(&e);
    }
    if (!hasEboot) {
        if (err) *err = "the download is not a VivoTube package";
        return false;
    }
    if (total) *total = (int)files.size() * 2;
    if (done) *done = 0;
    // 1. Unpack everything next to the current files.
    std::vector<std::string> written;
    auto cleanup = [&] {
        for (const std::string& p : written) remove(p.c_str());
    };
    for (const ZipEntry* e : files) {
        if (e->dataOffset + e->compSize > vpk.size()) {
            if (err) *err = "the download is incomplete";
            cleanup();
            return false;
        }
        const uint8_t* src = reinterpret_cast<const uint8_t*>(vpk.data()) + e->dataOffset;
        std::string data;
        if (e->method == 0) {
            data.assign(reinterpret_cast<const char*>(src), e->compSize);
        } else if (e->method == 8) {
            if (!inflate_raw(src, e->compSize, data, e->size, err)) {
                cleanup();
                return false;
            }
        } else {
            if (err) *err = "unsupported compression in " + e->name;
            cleanup();
            return false;
        }
        if (crc32_update(0, reinterpret_cast<const uint8_t*>(data.data()), data.size()) != e->crc) {
            if (err) *err = "checksum mismatch in " + e->name;
            cleanup();
            return false;
        }
        std::string target = appDir + "/" + e->name + ".new";
        make_dirs(parent_dir(target));
        if (int code = write_all(target, data)) {
            VT_LOG("update: writing %s failed: 0x%08X", target.c_str(), (unsigned)code);
            if (err) *err = str_printf("couldn't write %s (error 0x%08X)", target.c_str(), (unsigned)code);
            cleanup();
            return false;
        }
        written.push_back(target);
        if (done) (*done)++;
    }
    // 2. Swap them in. The system can't rename over an existing file, so remove it first.
    for (const ZipEntry* e : files) {
        std::string target = appDir + "/" + e->name;
        remove(target.c_str());
        if (rename((target + ".new").c_str(), target.c_str()) != 0) {
            if (err) *err = "couldn't replace " + target;
            return false;
        }
        if (done) (*done)++;
    }
    VT_LOG("update: installed %u files into %s", (unsigned)files.size(), appDir.c_str());
    return true;
}

bool can_update_in_place(const std::string& appDir) {
    std::string probe = appDir + "/.update_test";
    int code = write_all(probe, "ok");
    if (code == 0) sceIoRemove(probe.c_str());
    // VitaShell's test: ux0: refuses this request to homebrew that runs without extended rights.
    int devctl = sceIoDevctl("ux0:", 0x3001, nullptr, 0, nullptr, 0);
    VT_LOG("update: %s is %swritable (0x%08X); ux0 devctl 0x%08X", appDir.c_str(), code == 0 ? "" : "not ",
           (unsigned)code, (unsigned)devctl);
    return code == 0;
}

bool relaunch_app() {
    int r = sceAppMgrLoadExec("app0:eboot.bin", nullptr, nullptr);
    if (r < 0) VT_LOG("LoadExec: 0x%08X", (unsigned)r);
    return r >= 0;
}

}  // namespace vt
