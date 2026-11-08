#include "zip.h"

namespace vt {

namespace {

uint16_t rd16(const std::string& s, size_t o) { return (uint16_t)((uint8_t)s[o] | ((uint8_t)s[o + 1] << 8)); }
uint32_t rd32(const std::string& s, size_t o) { return (uint32_t)rd16(s, o) | ((uint32_t)rd16(s, o + 2) << 16); }

bool safe_name(const std::string& n) {
    if (n.empty() || n[0] == '/' || n[0] == '\\' || n.find(':') != std::string::npos) return false;
    size_t start = 0;
    while (start <= n.size()) {
        size_t end = n.find_first_of("/\\", start);
        if (end == std::string::npos) end = n.size();
        if (n.compare(start, end - start, "..") == 0 && end - start == 2) return false;
        if (end >= n.size()) break;
        start = end + 1;
    }
    return true;
}

}  // namespace

uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < len; i++) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

bool zip_entries(const std::string& a, std::vector<ZipEntry>& out, std::string* err) {
    auto fail = [&](const char* m) {
        if (err) *err = m;
        return false;
    };
    out.clear();
    if (a.size() < 22) return fail("not a zip archive");
    // The end-of-central-directory record sits in the last 64 KB + 22 bytes.
    size_t minPos = a.size() > 65557 ? a.size() - 65557 : 0;
    size_t eocd = std::string::npos;
    for (size_t p = a.size() - 22 + 1; p-- > minPos;) {
        if (rd32(a, p) == 0x06054b50) {
            eocd = p;
            break;
        }
    }
    if (eocd == std::string::npos) return fail("not a zip archive");
    uint16_t count = rd16(a, eocd + 10);
    uint32_t cdSize = rd32(a, eocd + 12), cdOff = rd32(a, eocd + 16);
    if (count == 0xFFFF || cdOff == 0xFFFFFFFFu) return fail("zip64 archives are not supported");
    if ((size_t)cdOff + cdSize > a.size()) return fail("truncated zip archive");
    size_t p = cdOff;
    for (uint16_t i = 0; i < count; i++) {
        if (p + 46 > a.size() || rd32(a, p) != 0x02014b50) return fail("bad zip directory");
        ZipEntry e;
        uint16_t flags = rd16(a, p + 8);
        e.method = rd16(a, p + 10);
        e.crc = rd32(a, p + 16);
        e.compSize = rd32(a, p + 20);
        e.size = rd32(a, p + 24);
        uint16_t nameLen = rd16(a, p + 28), extraLen = rd16(a, p + 30), commentLen = rd16(a, p + 32);
        uint32_t local = rd32(a, p + 42);
        if (p + 46 + nameLen > a.size()) return fail("bad zip directory");
        e.name = a.substr(p + 46, nameLen);
        p += 46 + nameLen + extraLen + commentLen;
        if (flags & 1) return fail("encrypted zip entries are not supported");
        if (!safe_name(e.name)) return fail("unsafe file name in archive");
        if (e.method != 0 && e.method != 8) return fail("unsupported zip compression");
        if ((size_t)local + 30 > a.size() || rd32(a, local) != 0x04034b50) return fail("bad zip entry");
        e.dataOffset = (size_t)local + 30 + rd16(a, local + 26) + rd16(a, local + 28);
        if (e.dataOffset + e.compSize > a.size()) return fail("truncated zip entry");
        out.push_back(e);
    }
    return true;
}

}  // namespace vt
