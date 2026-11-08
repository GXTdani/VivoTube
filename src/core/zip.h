// Minimal ZIP reader for installing update VPKs (a VPK is a ZIP archive). It only lists the
// entries and locates their data; inflating is left to the caller (zlib on the Vita).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vt {

struct ZipEntry {
    std::string name;
    uint16_t method = 0;  // 0 = stored, 8 = deflate
    uint32_t crc = 0, compSize = 0, size = 0;
    size_t dataOffset = 0;  // start of the entry's (compressed) bytes in the archive
    bool isDir() const { return !name.empty() && name.back() == '/'; }
};

// Reads the central directory. Rejects unsafe names ("..", absolute paths) and zip64.
bool zip_entries(const std::string& archive, std::vector<ZipEntry>& out, std::string* err);

uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len);  // start with 0

}  // namespace vt
