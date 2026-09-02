// =============================================================================
// Storage/FileSystem.cpp — LittleFS wrapper
// =============================================================================
#include "FileSystem.h"
#include "../Core/Config.h"
#include "../Core/Common.h"

namespace Storage {

FileSystem fs;

bool FileSystem::begin() {
  if (!LittleFS.begin(true)) {  // auto-format on first run
    Serial.println(F("[FS] LittleFS begin failed"));
    return false;
  }
  Serial.printf("[FS] mounted: %u / %u bytes used\n",
                (unsigned)usedBytes(), (unsigned)totalBytes());
  return true;
}

void FileSystem::end() { LittleFS.end(); }

String FileSystem::readAll(const char* path) const {
  File f = LittleFS.open(path, "r");
  if (!f) return String();
  String s;
  s.reserve(f.size());
  while (f.available()) {
    // Read in chunks to avoid heap fragmentation
    uint8_t buf[256];
    int n = f.read(buf, sizeof(buf));
    if (n > 0) s.concat((const char*)buf, n);
  }
  f.close();
  return s;
}

bool FileSystem::writeAll(const char* path, const String& data) {
  File f = LittleFS.open(path, "w");
  if (!f) return false;
  size_t written = f.print(data);
  f.close();
  return written == data.length();
}

bool FileSystem::atomicWrite(const char* path, const String& data) {
  String tmp = String(path) + ".tmp";
  String bak = String(path) + ".bak";

  // Step 1: write to .tmp
  if (!writeAll(tmp.c_str(), data)) return false;

  // Step 2: remove old .bak if exists
  if (LittleFS.exists(bak.c_str())) LittleFS.remove(bak.c_str());

  // Step 3: rename current → .bak (if exists)
  if (LittleFS.exists(path)) {
    if (!LittleFS.rename(path, bak.c_str())) return false;
  }
  // Step 4: rename .tmp → path
  if (!LittleFS.rename(tmp.c_str(), path)) {
    // Rollback: restore from .bak
    if (LittleFS.exists(bak.c_str())) LittleFS.rename(bak.c_str(), path);
    return false;
  }
  // Step 5: remove .bak
  if (LittleFS.exists(bak.c_str())) LittleFS.remove(bak.c_str());
  return true;
}

bool FileSystem::remove(const char* path) {
  return LittleFS.remove(path);
}

bool FileSystem::rename(const char* from, const char* to) {
  return LittleFS.rename(from, to);
}

size_t FileSystem::totalBytes() const { return LittleFS.totalBytes(); }
size_t FileSystem::usedBytes()  const { return LittleFS.usedBytes();  }

void FileSystem::cleanupTempFiles() {
  // Remove stale .tmp files (left over from interrupted atomic writes)
  LittleFS.remove("/config.tmp");
  LittleFS.remove("/calibration.tmp");
}

} // namespace Storage
