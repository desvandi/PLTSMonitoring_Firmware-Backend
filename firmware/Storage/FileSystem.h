// =============================================================================
// Storage/FileSystem.h — LittleFS wrapper with atomic write (.tmp → rename)
// =============================================================================
#pragma once
#ifndef PLTS_STORAGE_FILE_SYSTEM_H
#define PLTS_STORAGE_FILE_SYSTEM_H

#include <Arduino.h>
#include <LittleFS.h>

namespace Storage {

class FileSystem {
public:
  bool begin();
  void end();
  bool exists(const char* path) const { return LittleFS.exists(path); }
  String readAll(const char* path) const;
  bool writeAll(const char* path, const String& data);
  // Atomic write: write to path.tmp, then rename path → path.bak, then path.tmp → path
  bool atomicWrite(const char* path, const String& data);
  bool remove(const char* path);
  bool rename(const char* from, const char* to);
  size_t totalBytes() const;
  size_t usedBytes() const;
  void cleanupTempFiles();
  fs::FS& raw() { return LittleFS; }  // returns the LittleFS instance for direct access
};

extern FileSystem fs;

} // namespace Storage

#endif // PLTS_STORAGE_FILE_SYSTEM_H
